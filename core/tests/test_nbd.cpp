// SPDX-License-Identifier: Apache-2.0
// NBD クライアントのテスト。
//  - nbd_*: 自前の疑似サーバ (このファイル内、localhost の TCP / UNIX ソケット) に対するテスト。プロトコルの全経路と、故障・不正応答の注入。
//  - fixture_nbd_*: 実在のサーバ (nbdkit / qemu-nbd) と、nbdinfo / nbdcopy との独立照合。ツールが無ければ skip と表示して通す。
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "test_util.h"
#include "usbmem/disk_image.h"

extern std::string g_fixture_dir;
using namespace usbmem;
using Bytes = std::vector<uint8_t>;

namespace {

void be16w(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
void be32w(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void be64w(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i)); }
uint16_t be16r(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32r(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64r(const uint8_t* p) { return (uint64_t(be32r(p)) << 32) | be32r(p + 4); }

enum { T_READ = 0, T_WRITE = 1, T_DISC = 2, T_FLUSH = 3, T_TRIM = 4, T_ZEROES = 6 };
enum { TF_HAS = 1, TF_RO = 2, TF_FLUSH = 4, TF_FUA = 8, TF_TRIM = 32, TF_ZEROES = 64 };

struct Req { uint16_t type, flags; uint64_t off; uint32_t len; };

enum class Mut { None, BadHandle, ChunkOutOfRange, ShortCoverage, Overlap, BadMagic, UnknownChunkType };

struct FakeCfg {
  bool structured = true, use_go = true, no_zeroes = true, ro = false;
  uint32_t minb = 0, prefb = 0, maxb = 0;           // 0 = BLOCK_SIZE 情報を送らない
  uint16_t flag_mask = 0xFFFF;                       // 送る転送フラグ (FUA/TRIM/... を落とす用)
  std::string export_name;                           // 受け付ける名前
  std::atomic<int> delay_ms{0};
  std::atomic<int> drop_before_reply_at{-1};         // 1 始まりの要求番号。応答せずに切断 (1 回だけ)
  std::atomic<int> drop_mid_reply_at{-1};            // READ 応答の途中で切断 (1 回だけ)
  std::atomic<int> garbage_seed{0};                  // 非 0: 応答の代わりにランダムなバイト列
  std::atomic<int> garbage_in_handshake{0};          // 非 0: ハンドシェイク中にランダムなバイト列
  std::atomic<bool> garbage_keep_open{false};
  std::atomic<int> fail_errno{0};                    // 非 0: [fail_lo, fail_hi) と重なる READ/WRITE にこのエラー
  std::atomic<uint64_t> fail_lo{0}, fail_hi{0};
  std::atomic<Mut> mut{Mut::None};
  std::atomic<bool> split{false};
  std::atomic<bool> immediate_close{false}, silent{false}, oldstyle{false}, junk_greeting{false};
  std::atomic<bool> tls_required{false};
};

class FakeNbd {
 public:
  FakeNbd(size_t size, bool unix_mode = false, bool abstract_ns = false) : unix_mode_(unix_mode) {
    data.resize(size);
    std::mt19937_64 rng(size);
    for (size_t i = 0; i < size; i++) data[i] = (i / 4096) % 3 == 1 ? 0 : static_cast<uint8_t>(rng());   // 0 の領域も混ぜる (hole 試験)
    if (unix_mode) {
      path = (abstract_ns ? "@usbmem_nbd_test_" : test_tmpdir() + "/usbmem_nbd_test_") + std::to_string(getpid()) + "_" + std::to_string(counter()++);
      lfd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sun_family = AF_UNIX;
      std::memcpy(sa.sun_path, path.data(), path.size());
      socklen_t sl = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + (abstract_ns ? 0 : 1));
      if (abstract_ns) sa.sun_path[0] = '\0'; else ::unlink(path.c_str());
      if (::bind(lfd_, reinterpret_cast<sockaddr*>(&sa), sl) != 0) std::perror("bind unix");
    } else {
      lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      int one = 1;
      setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      sockaddr_in sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sin_family = AF_INET;
      sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::bind(lfd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) std::perror("bind tcp");
      socklen_t l = sizeof sa;
      getsockname(lfd_, reinterpret_cast<sockaddr*>(&sa), &l);
      port = ntohs(sa.sin_port);
    }
    ::listen(lfd_, 8);
    th_ = std::thread([this] { loop(); });
  }
  ~FakeNbd() {
    stop_ = true;
    th_.join();
    ::close(lfd_);
    if (unix_mode_ && path[0] != '@') ::unlink(path.c_str());
  }
  NbdOptions opts() const {
    NbdOptions o;
    if (unix_mode_) o.unix_socket = path; else { o.host = "127.0.0.1"; o.port = port; }
    o.export_name = cfg.export_name;
    o.io_timeout_ms = 3000;
    o.connect_timeout_ms = 3000;
    return o;
  }
  Bytes snapshot() { std::lock_guard<std::mutex> lk(mu); return data; }
  std::vector<Req> requests() { std::lock_guard<std::mutex> lk(mu); return log; }
  void clear_log() { std::lock_guard<std::mutex> lk(mu); log.clear(); }
  int connections() { return conns.load(); }

  FakeCfg cfg;
  Bytes data;
  std::vector<Req> log;
  std::mutex mu;
  std::atomic<int> conns{0}, flushes{0}, violations{0}, req_index{0};
  uint16_t port = 0;
  std::string path;

 private:
  static std::atomic<int>& counter() { static std::atomic<int> c{0}; return c; }
  bool unix_mode_;
  int lfd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread th_;

  void loop() {
    while (!stop_) {
      struct pollfd pf{lfd_, POLLIN, 0};
      if (::poll(&pf, 1, 50) <= 0) continue;
      int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) continue;
      conns++;
      serve(fd);
      ::close(fd);
    }
  }
  bool rd(int fd, void* p, size_t n) {
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n > 0) {
      if (stop_) return false;
      struct pollfd pf{fd, POLLIN, 0};
      int r = ::poll(&pf, 1, 50);
      if (r <= 0) continue;
      ssize_t k = ::recv(fd, b, n, 0);
      if (k <= 0) return false;
      b += k; n -= static_cast<size_t>(k);
    }
    return true;
  }
  bool wr(int fd, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (n > 0) {
      ssize_t k = ::send(fd, b, n, MSG_NOSIGNAL);
      if (k <= 0) return false;
      b += k; n -= static_cast<size_t>(k);
    }
    return true;
  }
  void sleep_ms(int ms) {
    for (int t = 0; t < ms && !stop_; t += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  bool opt_reply(int fd, uint32_t opt, uint32_t type, const Bytes& pl) {
    Bytes m(20 + pl.size());
    be64w(m.data(), 0x3e889045565a9ULL);
    be32w(m.data() + 8, opt);
    be32w(m.data() + 12, type);
    be32w(m.data() + 16, static_cast<uint32_t>(pl.size()));
    if (!pl.empty()) std::memcpy(m.data() + 20, pl.data(), pl.size());
    return wr(fd, m.data(), m.size());
  }
  void garbage(int fd, int seed) {
    std::mt19937_64 rng(static_cast<uint64_t>(seed));
    Bytes g(1 + rng() % 96);
    for (auto& b : g) b = static_cast<uint8_t>(rng());
    if (seed % 3 == 0) {   // 一部は「それらしい」先頭を持つゴミにする
      static const uint8_t sr[4] = {0x66, 0x8e, 0x33, 0xef}, sm[4] = {0x67, 0x44, 0x66, 0x98};
      if (g.size() >= 4) std::memcpy(g.data(), seed % 2 ? sr : sm, 4);
    }
    wr(fd, g.data(), g.size());
    if (!cfg.garbage_keep_open) return;
    sleep_ms(300);
  }
  uint16_t tflags() const {
    uint16_t f = TF_HAS | TF_FLUSH | TF_FUA | TF_TRIM | TF_ZEROES;
    f &= cfg.flag_mask;
    f |= TF_HAS;
    if (cfg.ro) f |= TF_RO;
    return f;
  }

  void serve(int fd) {
    if (cfg.immediate_close) return;
    if (cfg.silent) { uint8_t b; while (rd(fd, &b, 1)) {} return; }
    if (cfg.junk_greeting) { const char* j = "this is not nbd.."; wr(fd, j, 16); return; }
    if (cfg.oldstyle) {
      uint8_t g[8 + 8 + 8 + 4 + 124] = {0};
      std::memcpy(g, "NBDMAGIC", 8);
      be64w(g + 8, 0x00420281861253ULL);
      be64w(g + 16, data.size());
      wr(fd, g, sizeof g);
      return;
    }
    if (cfg.garbage_in_handshake) { garbage(fd, cfg.garbage_in_handshake); return; }
    uint8_t g[18];
    std::memcpy(g, "NBDMAGIC", 8);
    std::memcpy(g + 8, "IHAVEOPT", 8);
    be16w(g + 16, static_cast<uint16_t>(1 | (cfg.no_zeroes ? 2 : 0)));
    if (!wr(fd, g, 18)) return;
    uint8_t cf[4];
    if (!rd(fd, cf, 4)) return;
    bool noz = cfg.no_zeroes && (be32r(cf) & 2);
    bool sr = false;
    for (;;) {
      uint8_t h[16];
      if (!rd(fd, h, 16)) return;
      if (be64r(h) != 0x49484156454F5054ULL) return;
      uint32_t opt = be32r(h + 8), len = be32r(h + 12);
      if (len > (1u << 20)) return;
      Bytes d(len);
      if (len && !rd(fd, d.data(), len)) return;
      if (opt == 8) {   // STRUCTURED_REPLY
        if (cfg.structured) { sr = true; if (!opt_reply(fd, opt, 1, {})) return; }
        else if (!opt_reply(fd, opt, 0x80000001u, {})) return;
      } else if (opt == 3) {   // LIST
        for (const char* n : {"", "second"}) {
          Bytes pl(4 + std::strlen(n));
          be32w(pl.data(), static_cast<uint32_t>(std::strlen(n)));
          std::memcpy(pl.data() + 4, n, std::strlen(n));
          if (!opt_reply(fd, opt, 2, pl)) return;
        }
        if (!opt_reply(fd, opt, 1, {})) return;
      } else if (opt == 2) {   // ABORT
        opt_reply(fd, opt, 1, {});
        return;
      } else if (opt == 7 || opt == 6) {   // GO / INFO
        if (d.size() < 4) return;
        uint32_t nl = be32r(d.data());
        if (nl > d.size() - 4) return;
        std::string name(reinterpret_cast<char*>(d.data() + 4), nl);
        if (cfg.tls_required) { if (!opt_reply(fd, opt, 0x80000005u, {})) return; continue; }
        if (!cfg.use_go) { if (!opt_reply(fd, opt, 0x80000001u, {})) return; continue; }
        if (name != cfg.export_name) {
          const char* msg = "no such export";
          Bytes pl(msg, msg + std::strlen(msg));
          if (!opt_reply(fd, opt, 0x80000006u, pl)) return;
          continue;
        }
        Bytes i0(12);
        be16w(i0.data(), 0);
        be64w(i0.data() + 2, data.size());
        be16w(i0.data() + 10, tflags());
        if (!opt_reply(fd, opt, 3, i0)) return;
        if (cfg.maxb) {
          Bytes i3(14);
          be16w(i3.data(), 3);
          be32w(i3.data() + 2, cfg.minb);
          be32w(i3.data() + 6, cfg.prefb);
          be32w(i3.data() + 10, cfg.maxb);
          if (!opt_reply(fd, opt, 3, i3)) return;
        }
        if (!opt_reply(fd, opt, 1, {})) return;
        if (opt == 7) break;
      } else if (opt == 1) {   // EXPORT_NAME (旧式)
        std::string name(d.begin(), d.end());
        if (name != cfg.export_name) return;   // 仕様どおり、未知の名前は切断
        uint8_t r[10 + 124] = {0};
        be64w(r, data.size());
        be16w(r + 8, tflags());
        if (!wr(fd, r, noz ? 10 : 134)) return;
        break;
      } else if (!opt_reply(fd, opt, 0x80000001u, {})) return;
    }
    transmit(fd, sr);
  }

  void transmit(int fd, bool sr) {
    for (;;) {
      uint8_t h[28];
      if (!rd(fd, h, 28)) return;
      if (be32r(h) != 0x25609513u) return;
      Req r{be16r(h + 6), be16r(h + 4), be64r(h + 16), be32r(h + 24)};
      uint64_t handle = be64r(h + 8);
      int idx = ++req_index;
      {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(r);
      }
      if (r.type == T_DISC) return;
      Bytes payload;
      if (r.type == T_WRITE) {
        payload.resize(r.len);
        if (r.len && !rd(fd, payload.data(), r.len)) return;
      }
      sleep_ms(cfg.delay_ms);
      int db = cfg.drop_before_reply_at;
      if (db == idx) { cfg.drop_before_reply_at = -1; return; }
      if (cfg.garbage_seed) { garbage(fd, cfg.garbage_seed); return; }
      // 検証 (クライアントが制約を守っているか)
      uint32_t err = 0;
      bool is_io = r.type == T_READ || r.type == T_WRITE || r.type == T_TRIM || r.type == T_ZEROES;
      if (is_io) {
        if (r.off + r.len > data.size()) err = 22;
        if (cfg.minb && (r.off % cfg.minb || r.len % cfg.minb)) { err = 22; violations++; }
        if (cfg.maxb && (r.type == T_READ || r.type == T_WRITE) && r.len > cfg.maxb) { err = 22; violations++; }
      }
      if (!err && cfg.fail_errno && is_io && r.off < cfg.fail_hi && r.off + r.len > cfg.fail_lo) err = static_cast<uint32_t>(cfg.fail_errno.load());
      if (!err && r.type == T_WRITE && cfg.ro) err = 1;
      auto simple = [&](uint32_t e) {
        uint8_t m[16];
        be32w(m, 0x67446698u);
        be32w(m + 4, e);
        be64w(m + 8, handle);
        return wr(fd, m, 16);
      };
      auto chunk = [&](uint16_t flags, uint16_t type, const Bytes& pl, uint64_t hdl) {
        Bytes m(20 + pl.size());
        be32w(m.data(), cfg.mut == Mut::BadMagic ? 0x12345678u : 0x668e33efu);
        be16w(m.data() + 4, flags);
        be16w(m.data() + 6, type);
        be64w(m.data() + 8, hdl);
        be32w(m.data() + 16, static_cast<uint32_t>(pl.size()));
        if (!pl.empty()) std::memcpy(m.data() + 20, pl.data(), pl.size());
        return wr(fd, m.data(), m.size());
      };
      switch (r.type) {
        case T_READ: {
          if (err) {
            if (sr) {
              Bytes pl(6);
              be32w(pl.data(), err);
              if (!chunk(1, 0x8001, pl, handle)) return;
            } else if (!simple(err)) return;
            break;
          }
          Bytes d;
          {
            std::lock_guard<std::mutex> lk(mu);
            d.assign(data.begin() + static_cast<long>(r.off), data.begin() + static_cast<long>(r.off + r.len));
          }
          if (!sr) {
            if (cfg.drop_mid_reply_at == idx) {
              cfg.drop_mid_reply_at = -1;
              uint8_t m[16];
              be32w(m, 0x67446698u); be32w(m + 4, 0); be64w(m + 8, handle);
              wr(fd, m, 16);
              wr(fd, d.data(), d.size() / 2);
              return;
            }
            if (!simple(0) || !wr(fd, d.data(), d.size())) return;
            break;
          }
          Mut mt = cfg.mut;
          uint64_t hdl = mt == Mut::BadHandle ? handle + 1 : handle;
          if (cfg.drop_mid_reply_at == idx) {
            cfg.drop_mid_reply_at = -1;
            Bytes pl(8 + d.size());
            be64w(pl.data(), r.off);
            std::memcpy(pl.data() + 8, d.data(), d.size());
            Bytes m(20 + pl.size() / 2);
            be32w(m.data(), 0x668e33efu); be16w(m.data() + 4, 1); be16w(m.data() + 6, 1); be64w(m.data() + 8, handle); be32w(m.data() + 16, static_cast<uint32_t>(pl.size()));
            std::memcpy(m.data() + 20, pl.data(), pl.size() / 2);
            wr(fd, m.data(), m.size());
            return;
          }
          if (cfg.split || mt == Mut::ShortCoverage || mt == Mut::Overlap) {
            // 一定サイズごとに分割。全部 0 の区間は HOLE で送る
            std::mt19937_64 rng(r.off + 1);
            size_t pos = 0;
            std::vector<std::pair<size_t, size_t>> parts;
            while (pos < d.size()) {
              size_t n = std::min<size_t>(d.size() - pos, 1 + rng() % 5000);
              parts.push_back({pos, n});
              pos += n;
            }
            if (mt == Mut::ShortCoverage && parts.size() > 1) parts.pop_back();
            if (mt == Mut::ShortCoverage && parts.size() == 1) parts[0].second /= 2;
            if (mt == Mut::Overlap) parts.push_back(parts[0]);
            for (size_t i = 0; i < parts.size(); i++) {
              auto [p0, n] = parts[i];
              uint16_t fl = i + 1 == parts.size() ? 1 : 0;
              bool zero = std::all_of(d.begin() + static_cast<long>(p0), d.begin() + static_cast<long>(p0 + n), [](uint8_t b) { return b == 0; });
              if (zero) {
                Bytes pl(12);
                be64w(pl.data(), r.off + p0);
                be32w(pl.data() + 8, static_cast<uint32_t>(n));
                if (!chunk(fl, 2, pl, hdl)) return;
              } else {
                Bytes pl(8 + n);
                be64w(pl.data(), r.off + p0);
                std::memcpy(pl.data() + 8, d.data() + p0, n);
                if (!chunk(fl, 1, pl, hdl)) return;
              }
            }
            break;
          }
          Bytes pl(8 + d.size());
          be64w(pl.data(), mt == Mut::ChunkOutOfRange ? r.off + 1 : r.off);
          std::memcpy(pl.data() + 8, d.data(), d.size());
          if (mt == Mut::UnknownChunkType) { if (!chunk(1, 77, pl, hdl)) return; break; }
          if (!chunk(1, 1, pl, hdl)) return;
          break;
        }
        case T_WRITE:
          if (!err) {
            std::lock_guard<std::mutex> lk(mu);
            std::memcpy(data.data() + r.off, payload.data(), r.len);
          }
          if (!simple(err)) return;
          break;
        case T_FLUSH:
          flushes++;
          if (!simple(0)) return;
          break;
        case T_TRIM:
          if (!simple(err)) return;
          break;
        case T_ZEROES:
          if (!err) { std::lock_guard<std::mutex> lk(mu); std::memset(data.data() + r.off, 0, r.len); }
          if (!simple(err)) return;
          break;
        default:
          if (!simple(22)) return;
      }
    }
  }
};

std::shared_ptr<NbdDevice> conn(FakeNbd& s, NbdOptions o, Status* stp = nullptr, std::string* detail = nullptr) {
  std::shared_ptr<NbdDevice> d;
  Status st = NbdDevice::connect(o, &d, detail);
  if (stp) *stp = st;
  return st.ok() ? d : nullptr;
}
std::shared_ptr<NbdDevice> conn(FakeNbd& s) { return conn(s, s.opts()); }

bool eq(const Bytes& a, uint64_t ao, const Bytes& b, uint64_t bo, size_t n) { return std::memcmp(a.data() + ao, b.data() + bo, n) == 0; }

}  // namespace

// ---------------------------------------------------------------- 基本: 構造化応答 + OPT_GO
TEST(nbd_basic_structured) {
  FakeNbd s(4u << 20);
  Status st;
  std::string detail;
  auto d = conn(s, s.opts(), &st, &detail);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_EQ(d->size(), 4u << 20);
  CHECK(d->structured_replies());
  CHECK(!d->is_read_only());
  CHECK((d->transmission_flags() & TF_FUA) != 0);
  Bytes model = s.snapshot(), buf;
  std::mt19937_64 rng(3);
  for (int i = 0; i < 200; i++) {
    size_t len = 1 + rng() % 100000;
    uint64_t off = rng() % (model.size() - len + 1);
    buf.resize(len);
    CHECK(d->read_at(off, buf.data(), len).ok());
    CHECK(eq(buf, 0, model, off, len));
  }
  for (int i = 0; i < 50; i++) {
    size_t len = 1 + rng() % 70000;
    uint64_t off = rng() % (model.size() - len + 1);
    buf.resize(len);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    CHECK(d->write_at(off, buf.data(), len).ok());
    std::memcpy(model.data() + off, buf.data(), len);
  }
  CHECK(s.snapshot() == model);                 // サーバ側の中身が一致
  Bytes all(model.size());
  CHECK(d->read_at(0, all.data(), all.size()).ok());
  CHECK(all == model);
  for (const Req& r : s.requests()) if (r.type == T_WRITE) CHECK((r.flags & 1) != 0);   // 全書き込みに FUA
  CHECK(d->flush().ok());
  CHECK_EQ(s.flushes.load(), 1);
  uint8_t b = 0;
  CHECK(d->read_at(model.size(), &b, 1).code == Errc::OutOfRange);
  CHECK(d->write_at(model.size() - 1, &b, 2).code == Errc::OutOfRange);
  CHECK(d->read_at(0, &b, 0).ok());
  CHECK_EQ(d->reconnect_count(), 0u);
  CHECK_EQ(s.connections(), 1);
  CHECK(d->close().ok());
  CHECK(d->read_at(0, &b, 1).code == Errc::Closed);
  CHECK(d->close().ok());
}

TEST(nbd_unix_and_abstract_sockets) {
  for (bool abs : {false, true}) {
    FakeNbd s(1u << 20, true, abs);
    auto d = conn(s);
    CHECK(d != nullptr);
    if (!d) continue;
    Bytes model = s.snapshot(), buf(70000);
    CHECK(d->read_at(12345, buf.data(), buf.size()).ok());
    CHECK(eq(buf, 0, model, 12345, buf.size()));
  }
}

// ---------------------------------------------------------------- 旧式 EXPORT_NAME + 単純応答
TEST(nbd_export_name_and_simple_replies) {
  for (bool noz : {true, false}) {
    FakeNbd s(2u << 20);
    s.cfg.structured = false;
    s.cfg.use_go = false;
    s.cfg.no_zeroes = noz;
    s.cfg.export_name = "disk1";
    NbdOptions o = s.opts();
    auto d = conn(s, o);
    CHECK(d != nullptr);
    if (!d) continue;
    CHECK(!d->structured_replies());
    Bytes model = s.snapshot(), buf(200000);
    CHECK(d->read_at(777, buf.data(), buf.size()).ok());
    CHECK(eq(buf, 0, model, 777, buf.size()));
    Bytes w(5000, 0x5a);
    CHECK(d->write_at(100, w.data(), w.size()).ok());
    CHECK(std::memcmp(s.snapshot().data() + 100, w.data(), w.size()) == 0);
    // 名前違い → サーバは切断する (EXPORT_NAME は理由を返せない)。疑似サーバは 1 接続ずつなので先に閉じる
    d->close();
    o.export_name = "nope";
    Status st;
    std::string detail;
    CHECK(conn(s, o, &st, &detail) == nullptr);
    if (st.code != Errc::Disconnected) std::fprintf(stderr, "  got %s [%s]\n", errc_name(st.code), detail.c_str());
    CHECK(st.code == Errc::Disconnected);
  }
}

// ---------------------------------------------------------------- ブロックサイズ制約 (最小・最大)
TEST(nbd_block_size_limits) {
  FakeNbd s(8u << 20);
  s.cfg.minb = 4096; s.cfg.prefb = 65536; s.cfg.maxb = 65536;
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_EQ(d->min_block_size(), 4096u);
  CHECK_EQ(d->max_block_size(), 65536u);
  Bytes model = s.snapshot(), buf;
  std::mt19937_64 rng(8);
  for (int i = 0; i < 150; i++) {
    size_t len = 1 + rng() % 300000;
    uint64_t off = rng() % (model.size() - len + 1);
    buf.resize(len);
    CHECK(d->read_at(off, buf.data(), len).ok());
    CHECK(eq(buf, 0, model, off, len));
    if (i % 3 == 0) {
      for (auto& b : buf) b = static_cast<uint8_t>(rng());
      CHECK(d->write_at(off, buf.data(), len).ok());   // 端が揃っていなくても RMW で成功する
      std::memcpy(model.data() + off, buf.data(), len);
    }
  }
  CHECK(s.snapshot() == model);
  CHECK_EQ(s.violations.load(), 0);
  for (const Req& r : s.requests()) {
    if (r.type == T_READ || r.type == T_WRITE) { CHECK(r.len <= 65536); CHECK_EQ(r.off % 4096, 0u); CHECK_EQ(r.len % 4096, 0u); }
  }
  // 最大が小さい (512) 場合でも大きな転送が分割される
  FakeNbd s2(1u << 20);
  s2.cfg.minb = 1; s2.cfg.prefb = 512; s2.cfg.maxb = 512;
  auto d2 = conn(s2);
  CHECK(d2 != nullptr);
  if (d2) {
    Bytes m2 = s2.snapshot(), b2(100000);
    CHECK(d2->read_at(10, b2.data(), b2.size()).ok());
    CHECK(eq(b2, 0, m2, 10, b2.size()));
    CHECK_EQ(s2.violations.load(), 0);
  }
  // 最大が最小より小さいなどの不正な通知は無視される (制約なし扱い)
  FakeNbd s3(1u << 20);
  s3.cfg.minb = 3; s3.cfg.prefb = 4; s3.cfg.maxb = 2;
  auto d3 = conn(s3);
  CHECK(d3 != nullptr);
  if (d3) { CHECK_EQ(d3->min_block_size(), 1u); }
}

TEST(nbd_structured_chunks_and_holes) {
  FakeNbd s(4u << 20);
  s.cfg.split = true;
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  Bytes model = s.snapshot(), buf(model.size());
  CHECK(d->read_at(0, buf.data(), buf.size()).ok());
  CHECK(buf == model);
  std::mt19937_64 rng(4);
  for (int i = 0; i < 100; i++) {
    size_t len = 1 + rng() % 50000;
    uint64_t off = rng() % (model.size() - len + 1);
    CHECK(d->read_at(off, buf.data(), len).ok());
    CHECK(eq(buf, 0, model, off, len));
  }
}

// ---------------------------------------------------------------- エラー応答
TEST(nbd_error_replies) {
  for (bool sr : {true, false}) {
    FakeNbd s(2u << 20);
    s.cfg.structured = sr;
    auto d = conn(s);
    CHECK(d != nullptr);
    if (!d) continue;
    Bytes buf(4096);
    s.cfg.fail_lo = 4096 * 10; s.cfg.fail_hi = 4096 * 11;
    for (auto [e, want] : std::vector<std::pair<int, Errc>>{{5, Errc::Io}, {28, Errc::Io}, {1, Errc::ReadOnly}, {95, Errc::Unsupported}, {12, Errc::NoMemory},
                                                             {22, Errc::InvalidArgument}, {108, Errc::Disconnected}, {75, Errc::OutOfRange}, {77, Errc::Io}}) {
      s.cfg.fail_errno = e;
      Status st = d->read_at(4096 * 10, buf.data(), buf.size());
      CHECK(st.code == want);
      if (want == Errc::Io) CHECK_EQ(st.sys_errno, e);
      CHECK(d->read_at(0, buf.data(), buf.size()).ok());   // エラー応答のあとも接続は生きている
      Status w = d->write_at(4096 * 10, buf.data(), buf.size());
      CHECK(w.code == want);
    }
    CHECK_EQ(s.connections(), 1);
    CHECK_EQ(d->reconnect_count(), 0u);
  }
  // 存在しないエクスポート
  {
    FakeNbd s(1u << 20);
    s.cfg.export_name = "real";
    NbdOptions o = s.opts();
    o.export_name = "fake";
    Status st;
    std::string detail;
    CHECK(conn(s, o, &st, &detail) == nullptr);
    CHECK(st.code == Errc::InvalidArgument);
    CHECK(detail.find("fake") != std::string::npos);
  }
  // TLS 必須のサーバ
  {
    FakeNbd s(1u << 20);
    s.cfg.tls_required = true;
    Status st;
    std::string detail;
    CHECK(conn(s, s.opts(), &st, &detail) == nullptr);
    CHECK(st.code == Errc::Unsupported);
    CHECK(detail.find("TLS") != std::string::npos);
  }
  // 読み取り専用: サーバが ro / クライアントが ro → 書き込みはネットワークに出さずに ReadOnly
  for (int mode = 0; mode < 2; mode++) {
    FakeNbd s(1u << 20);
    s.cfg.ro = mode == 0;
    NbdOptions o = s.opts();
    o.read_only = mode == 1;
    auto d = conn(s, o);
    CHECK(d != nullptr);
    if (!d) continue;
    CHECK(d->is_read_only());
    Bytes b(100, 1);
    CHECK(d->write_at(0, b.data(), b.size()).code == Errc::ReadOnly);
    CHECK(d->trim(0, 4096).code == Errc::ReadOnly);
    CHECK(d->write_zeroes(0, 4096).code == Errc::ReadOnly);
    CHECK(d->flush().ok());
    for (const Req& r : s.requests()) CHECK(r.type != T_WRITE && r.type != T_TRIM && r.type != T_ZEROES);
  }
}

// ---------------------------------------------------------------- タイムアウト・キャンセルと復帰
TEST(nbd_timeout_and_recovery) {
  FakeNbd s(2u << 20);
  NbdOptions o = s.opts();
  o.io_timeout_ms = 150;
  auto d = conn(s, o);
  CHECK(d != nullptr);
  if (!d) return;
  Bytes buf(4096);
  CHECK(d->read_at(0, buf.data(), buf.size()).ok());
  s.cfg.delay_ms = 600;
  auto t0 = std::chrono::steady_clock::now();
  CHECK(d->read_at(0, buf.data(), buf.size()).code == Errc::Timeout);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms >= 100 && ms < 500);
  // IoOptions のタイムアウトが優先される
  IoOptions io;
  io.timeout_ms = 2000;
  s.cfg.delay_ms = 400;
  // 前回の応答待ちで切った接続が残っているので、サーバが空くのを待ってから
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  CHECK(d->read_at(0, buf.data(), buf.size(), io).ok());     // 400ms 遅いが、2 秒まで待つ → 成功
  s.cfg.delay_ms = 0;
  CHECK(d->read_at(0, buf.data(), buf.size()).ok());
  CHECK(d->reconnect_count() >= 1u);                          // タイムアウトで切った接続を張り直した
  Bytes model = s.snapshot();
  CHECK(std::memcmp(buf.data(), model.data(), buf.size()) == 0);   // 取り違え (古い応答を次の要求の応答と誤認) が無い
}

TEST(nbd_cancel) {
  FakeNbd s(2u << 20);
  s.cfg.delay_ms = 3000;
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  CancelToken tok;
  IoOptions io;
  io.cancel = &tok;
  std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); tok.cancel(); });
  Bytes buf(4096);
  auto t0 = std::chrono::steady_clock::now();
  CHECK(d->read_at(0, buf.data(), buf.size(), io).code == Errc::Cancelled);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  t.join();
  CHECK(ms < 1000);
  // すでにキャンセル済み → 何も送らず Cancelled
  CHECK(d->read_at(0, buf.data(), buf.size(), io).code == Errc::Cancelled);
  s.cfg.delay_ms = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(d->read_at(0, buf.data(), buf.size()).ok());   // 新しい要求は復帰して成功
  Bytes model = s.snapshot();
  CHECK(std::memcmp(buf.data(), model.data(), buf.size()) == 0);
}

// ---------------------------------------------------------------- 再接続と冪等な再試行
TEST(nbd_reconnect_idempotent_retry) {
  for (bool sr : {true, false}) {
    for (int where = 0; where < 2; where++) {   // 0: 応答前に切断 / 1: 応答の途中で切断
      FakeNbd s(2u << 20);
      s.cfg.structured = sr;
      auto d = conn(s);
      CHECK(d != nullptr);
      if (!d) continue;
      Bytes model = s.snapshot(), buf(100000);
      CHECK(d->read_at(0, buf.data(), 10).ok());
      (where == 0 ? s.cfg.drop_before_reply_at : s.cfg.drop_mid_reply_at) = s.req_index.load() + 1;
      CHECK(d->read_at(5000, buf.data(), buf.size()).ok());          // 利用者には見えない
      CHECK(eq(buf, 0, model, 5000, buf.size()));
      CHECK_EQ(d->reconnect_count(), 1u);
      CHECK_EQ(s.connections(), 2);
      // 書き込み (FUA つき) の再送
      if (where == 0) {
        s.cfg.drop_before_reply_at = s.req_index.load() + 1;
        Bytes w(30000, 0x77);
        CHECK(d->write_at(40000, w.data(), w.size()).ok());
        std::memset(model.data() + 40000, 0x77, w.size());
        CHECK(s.snapshot() == model);
        CHECK_EQ(d->reconnect_count(), 2u);
        CHECK(d->flush().ok());                                       // FUA なので失われた書き込みは無い
      }
    }
  }
  // 再接続しない設定
  {
    FakeNbd s(1u << 20);
    NbdOptions o = s.opts();
    o.auto_reconnect = false;
    auto d = conn(s, o);
    CHECK(d != nullptr);
    if (!d) return;
    Bytes b(100);
    s.cfg.drop_before_reply_at = s.req_index.load() + 1;
    CHECK(d->read_at(0, b.data(), b.size()).code == Errc::Disconnected);
    CHECK(d->read_at(0, b.data(), b.size()).code == Errc::Disconnected);   // 以後も Disconnected
    CHECK_EQ(s.connections(), 1);
  }
  // FUA 無しの書き込み: 切断をまたいで失われたかもしれない書き込みを、次の flush() が Disconnected で知らせる
  {
    FakeNbd s(1u << 20);
    NbdOptions o = s.opts();
    o.fua_writes = false;
    auto d = conn(s, o);
    CHECK(d != nullptr);
    if (!d) return;
    Bytes w(1000, 9);
    CHECK(d->write_at(0, w.data(), w.size()).ok());              // 未 flush
    for (const Req& r : s.requests()) if (r.type == T_WRITE) CHECK((r.flags & 1) == 0);
    s.cfg.drop_before_reply_at = s.req_index.load() + 1;
    CHECK(d->write_at(5000, w.data(), w.size()).ok());           // 再接続して再送は成功する
    CHECK(d->flush().code == Errc::Disconnected);                // が、最初の書き込みの耐久性は保証できない
    CHECK(d->flush().ok());                                      // 通知は 1 回だけ
  }
}

// ---------------------------------------------------------------- プロトコル違反・不正な相手
TEST(nbd_protocol_violations) {
  FakeNbd s(1u << 20);
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  Bytes buf(30000), model = s.snapshot();
  for (Mut m : {Mut::BadHandle, Mut::ChunkOutOfRange, Mut::ShortCoverage, Mut::Overlap, Mut::BadMagic, Mut::UnknownChunkType}) {
    s.cfg.mut = m;
    CHECK(d->read_at(1000, buf.data(), buf.size()).code == Errc::Corrupt);
    s.cfg.mut = Mut::None;
    CHECK(d->read_at(1000, buf.data(), buf.size()).ok());           // 壊れた接続は捨てて、次で復帰
    CHECK(eq(buf, 0, model, 1000, buf.size()));
  }
  CHECK(d->reconnect_count() >= 6u);
  // ハンドシェイク段階
  {
    FakeNbd j(4096);
    j.cfg.junk_greeting = true;
    Status st;
    CHECK(conn(j, j.opts(), &st) == nullptr);
    CHECK(st.code == Errc::Corrupt);
  }
  {
    FakeNbd o(4096);
    o.cfg.oldstyle = true;
    Status st;
    std::string detail;
    CHECK(conn(o, o.opts(), &st, &detail) == nullptr);
    CHECK(st.code == Errc::Unsupported);
    CHECK(!detail.empty());
  }
  {
    FakeNbd c(4096);
    c.cfg.immediate_close = true;
    Status st;
    CHECK(conn(c, c.opts(), &st) == nullptr);
    CHECK(st.code == Errc::Disconnected);
  }
  {
    FakeNbd q(4096);
    q.cfg.silent = true;
    NbdOptions o = q.opts();
    o.connect_timeout_ms = 200;
    Status st;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(conn(q, o, &st) == nullptr);
    CHECK(st.code == Errc::Timeout);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
  }
  {
    // 接続拒否: 閉じたポート
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
    socklen_t l = sizeof sa;
    getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &l);
    uint16_t port = ntohs(sa.sin_port);
    ::close(fd);
    NbdOptions o;
    o.host = "127.0.0.1";
    o.port = port;
    std::shared_ptr<NbdDevice> dd;
    std::string detail;
    Status st = NbdDevice::connect(o, &dd, &detail);
    CHECK(st.code == Errc::Io);
    CHECK(!detail.empty());
    o.host = "";
    CHECK(NbdDevice::connect(o, &dd, &detail).code == Errc::InvalidArgument);
    o.unix_socket = test_tmpdir() + "/usbmem_no_such_socket";
    CHECK(NbdDevice::connect(o, &dd, &detail).code == Errc::Io);
  }
}

// ---------------------------------------------------------------- TRIM / WRITE_ZEROES
TEST(nbd_trim_and_write_zeroes) {
  FakeNbd s(2u << 20);
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  Bytes model = s.snapshot();
  CHECK(d->write_zeroes(5000, 100000).ok());
  std::memset(model.data() + 5000, 0, 100000);
  CHECK(s.snapshot() == model);
  CHECK(d->trim(200000, 300000).ok());
  bool saw_zero = false, saw_trim = false;
  for (const Req& r : s.requests()) { saw_zero |= r.type == T_ZEROES; saw_trim |= r.type == T_TRIM; }
  CHECK(saw_zero && saw_trim);
  CHECK(d->write_zeroes(2u << 20, 1).code == Errc::OutOfRange);
  // 非対応のサーバ
  FakeNbd s2(1u << 20);
  s2.cfg.flag_mask = static_cast<uint16_t>(~(TF_TRIM | TF_ZEROES));
  auto d2 = conn(s2);
  CHECK(d2 != nullptr);
  if (d2) {
    CHECK(d2->trim(0, 4096).code == Errc::Unsupported);
    CHECK(d2->write_zeroes(0, 4096).code == Errc::Unsupported);
  }
  // 最小ブロックに揃わない WRITE_ZEROES は通常の書き込みで端を処理する
  FakeNbd s3(1u << 20);
  s3.cfg.minb = 4096; s3.cfg.prefb = 4096; s3.cfg.maxb = 1u << 20;
  auto d3 = conn(s3);
  CHECK(d3 != nullptr);
  if (d3) {
    Bytes m3 = s3.snapshot();
    CHECK(d3->write_zeroes(1000, 20000).ok());
    std::memset(m3.data() + 1000, 0, 20000);
    CHECK(s3.snapshot() == m3);
    CHECK_EQ(s3.violations.load(), 0);
    CHECK(d3->trim(100, 20000).ok());   // 揃わない端は捨てる
    CHECK_EQ(s3.violations.load(), 0);
  }
  // FUA 非対応のサーバでは FUA を付けない
  FakeNbd s4(1u << 20);
  s4.cfg.flag_mask = static_cast<uint16_t>(~TF_FUA);
  auto d4 = conn(s4);
  CHECK(d4 != nullptr);
  if (d4) {
    Bytes w(100, 1);
    CHECK(d4->write_at(0, w.data(), w.size()).ok());
    for (const Req& r : s4.requests()) if (r.type == T_WRITE) CHECK((r.flags & 1) == 0);
    CHECK(d4->flush().ok());
    CHECK_EQ(s4.flushes.load(), 1);
  }
}

TEST(nbd_list_exports) {
  FakeNbd s(4096);
  std::vector<std::string> names;
  std::string detail;
  CHECK(NbdDevice::list_exports(s.opts(), &names, &detail).ok());
  CHECK_EQ(names.size(), 2u);
  if (names.size() == 2) { CHECK(names[0] == ""); CHECK(names[1] == "second"); }
}

// ---------------------------------------------------------------- 大きな転送と並行アクセス
TEST(nbd_large_and_threads) {
  FakeNbd s(16u << 20);
  auto d = conn(s);
  CHECK(d != nullptr);
  if (!d) return;
  Bytes model = s.snapshot();
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++)
    th.emplace_back([&, t] {
      std::mt19937_64 rng(50 + t);
      uint64_t lo = static_cast<uint64_t>(t) * (4u << 20), hi = lo + (4u << 20);
      Bytes buf;
      for (int i = 0; i < 60; i++) {
        size_t len = 1 + rng() % 300000;
        uint64_t off = lo + rng() % (hi - lo - len + 1);
        buf.resize(len);
        if (i % 2) {
          for (auto& b : buf) b = static_cast<uint8_t>(rng());
          if (!d->write_at(off, buf.data(), len).ok()) bad++;
          std::memcpy(model.data() + off, buf.data(), len);     // 領域は分かれている
        } else {
          if (!d->read_at(off, buf.data(), len).ok() || std::memcmp(buf.data(), model.data() + off, len) != 0) bad++;
        }
      }
    });
  for (auto& t : th) t.join();
  CHECK_EQ(bad.load(), 0);
  CHECK(s.snapshot() == model);
  Bytes all(model.size());
  CHECK(d->read_at(0, all.data(), all.size()).ok());
  CHECK(all == model);
}

// ---------------------------------------------------------------- ゴミ応答を返す相手 (クラッシュ・ハングしない)
TEST(nbd_garbage_peer) {
  auto allowed = [](Errc c) { return c == Errc::Corrupt || c == Errc::Disconnected || c == Errc::Io || c == Errc::Timeout || c == Errc::Unsupported || c == Errc::InvalidArgument; };
  int in_hs = 0, in_tx = 0;
  for (int seed = 1; seed <= 150; seed++) {
    FakeNbd s(1u << 20);
    NbdOptions o = s.opts();
    o.io_timeout_ms = 250;
    o.connect_timeout_ms = 250;
    s.cfg.garbage_keep_open = seed % 25 == 0;
    if (seed % 3 == 0) {
      s.cfg.garbage_in_handshake = seed;
      Status st;
      auto d = conn(s, o, &st);
      CHECK(d == nullptr);
      CHECK(allowed(st.code));
      in_hs++;
    } else {
      auto d = conn(s, o);
      CHECK(d != nullptr);
      if (!d) continue;
      s.cfg.garbage_seed = seed;
      Bytes b(20000);
      Status st = d->read_at(0, b.data(), b.size());
      CHECK(!st.ok() && allowed(st.code));
      st = d->write_at(0, b.data(), b.size());
      CHECK(!st.ok() && allowed(st.code));
      st = d->flush();
      CHECK(!st.ok() && allowed(st.code));
      in_tx++;
    }
  }
  std::fprintf(stderr, "  garbage peer: %d handshake + %d transmission cases, no crash/hang\n", in_hs, in_tx);
}

// ======================================================================== 実サーバとの照合
namespace {

std::string find_tool(const char* name) {
  std::vector<std::string> dirs;
  if (const char* p = std::getenv("PATH")) {
    std::string s = p;
    size_t i = 0;
    while (i <= s.size()) { size_t j = s.find(':', i); if (j == std::string::npos) j = s.size(); dirs.push_back(s.substr(i, j - i)); i = j + 1; }
  }
  if (const char* h = std::getenv("HOME")) dirs.push_back(std::string(h) + "/.local/bin");
  dirs.push_back("/usr/bin");
  dirs.push_back("/usr/local/bin");
  for (auto& d : dirs) {
    std::string p = d + "/" + name;
    if (!d.empty() && access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

class Proc {
 public:
  Proc(const std::vector<std::string>& argv, const std::string& log = "/dev/null") {
    pid_ = ::fork();
    if (pid_ == 0) {
      int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); }
      int in = ::open("/dev/null", O_RDONLY);
      if (in >= 0) ::dup2(in, 0);
      std::vector<char*> a;
      for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
      a.push_back(nullptr);
      ::execv(a[0], a.data());
      _exit(127);
    }
  }
  ~Proc() { stop(); }
  void stop() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      for (int i = 0; i < 100; i++) {
        int st;
        if (::waitpid(pid_, &st, WNOHANG) == pid_) { pid_ = -1; return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }
  bool alive() const {
    if (pid_ <= 0) return false;
    int st;
    return ::waitpid(pid_, &st, WNOHANG) == 0;
  }
 private:
  pid_t pid_ = -1;
};

bool wait_path(const std::string& p, int ms = 8000) {
  for (int t = 0; t < ms; t += 20) {
    if (access(p.c_str(), F_OK) == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string run_capture(const std::string& cmd, int* rc = nullptr) {
  std::string out;
  FILE* f = ::popen(cmd.c_str(), "r");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  int r = ::pclose(f);
  if (rc) *rc = r;
  return out;
}

Bytes read_whole(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Ext {
  std::string dir, nbdkit, qemu_nbd, nbdinfo, nbdcopy;
  Ext(const char* tag) {
    nbdkit = find_tool("nbdkit"); qemu_nbd = find_tool("qemu-nbd"); nbdinfo = find_tool("nbdinfo"); nbdcopy = find_tool("nbdcopy");
    dir = test_tmpdir() + "/usbmem_nbd_ext_" + tag + "_" + std::to_string(getpid());
    ::mkdir(dir.c_str(), 0700);
  }
  ~Ext() { run_capture("rm -rf '" + dir + "'"); }
  std::string sock() const { return dir + "/s.sock"; }
  std::string uri() const { return "nbd+unix:///?socket=" + sock(); }
  NbdOptions opts() const { NbdOptions o; o.unix_socket = sock(); o.io_timeout_ms = 5000; o.connect_timeout_ms = 5000; return o; }
};

bool have(const std::string& tool, const char* what) {
  if (tool.empty()) { std::fprintf(stderr, "  (skip: %s not found)\n", what); return false; }
  return true;
}

}  // namespace

TEST(fixture_nbd_nbdkit_file_readonly) {
  Ext e("file");
  if (!have(e.nbdkit, "nbdkit") || g_fixture_dir.empty()) return;
  std::string raw = g_fixture_dir + "/main_mbr.raw";
  Bytes want = read_whole(raw);
  if (want.empty()) { std::fprintf(stderr, "  (skip: no fixture)\n"); return; }
  for (int variant = 0; variant < 3; variant++) {   // 0: 構造化応答 / 1: 単純応答のみ / 2: TCP
    std::vector<std::string> args = {e.nbdkit, "-f", "-r"};
    NbdOptions o = e.opts();
    if (variant == 2) {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
      socklen_t l = sizeof sa;
      getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &l);
      uint16_t port = ntohs(sa.sin_port);
      ::close(fd);
      args.insert(args.end(), {"-i", "127.0.0.1", "-p", std::to_string(port)});
      o.unix_socket.clear(); o.host = "127.0.0.1"; o.port = port;
    } else args.insert(args.end(), {"-U", e.sock()});
    if (variant == 1) args.push_back("--no-sr");
    args.insert(args.end(), {"file", "file=" + raw});
    Proc p(args, e.dir + "/nbdkit.log");
    bool up = false;
    for (int t = 0; t < 8000 && !up; t += 50) {
      std::shared_ptr<NbdDevice> d;
      if (NbdDevice::connect(o, &d).ok()) up = true; else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(up);
    std::shared_ptr<NbdDevice> d;
    Status st = NbdDevice::connect(o, &d);
    CHECK(st.ok());
    if (!d) continue;
    CHECK(d->is_read_only());
    CHECK_EQ(d->size(), want.size());
    CHECK_EQ(d->structured_replies(), variant != 1);
    Bytes got(want.size());
    CHECK(d->read_at(0, got.data(), got.size()).ok());
    CHECK(got == want);
    std::mt19937_64 rng(variant);
    for (int i = 0; i < 300; i++) {
      size_t len = 1 + rng() % 200000;
      uint64_t off = rng() % (want.size() - len + 1);
      CHECK(d->read_at(off, got.data(), len).ok());
      CHECK(std::memcmp(got.data(), want.data() + off, len) == 0);
    }
    uint8_t b = 1;
    CHECK(d->write_at(0, &b, 1).code == Errc::ReadOnly);
    if (variant == 0 && !e.nbdinfo.empty()) {
      std::string sz = run_capture(e.nbdinfo + " --size '" + e.uri() + "'");
      CHECK_EQ(std::strtoull(sz.c_str(), nullptr, 10), want.size());
    }
    d->close();
    p.stop();
    ::unlink(e.sock().c_str());
  }
}

TEST(fixture_nbd_nbdkit_memory_write_and_nbdcopy) {
  Ext e("mem");
  if (!have(e.nbdkit, "nbdkit")) return;
  const size_t N = 24u << 20;
  Proc p({e.nbdkit, "-f", "-U", e.sock(), "memory", std::to_string(N)}, e.dir + "/nbdkit.log");
  CHECK(wait_path(e.sock()));
  auto d = [&] { std::shared_ptr<NbdDevice> x; NbdDevice::connect(e.opts(), &x); return x; }();
  CHECK(d != nullptr);
  if (!d) return;
  CHECK_EQ(d->size(), N);
  Bytes model(N, 0), buf;
  std::mt19937_64 rng(77);
  for (int i = 0; i < 120; i++) {
    size_t len = 1 + rng() % 400000;
    uint64_t off = rng() % (N - len + 1);
    buf.resize(len);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    CHECK(d->write_at(off, buf.data(), len).ok());
    std::memcpy(model.data() + off, buf.data(), len);
  }
  CHECK(d->write_zeroes(1u << 20, 3u << 20).ok());
  std::memset(model.data() + (1u << 20), 0, 3u << 20);
  CHECK(d->trim(8u << 20, 1u << 20).ok());   // memory プラグインの TRIM は内容をゼロにする
  std::memset(model.data() + (8u << 20), 0, 1u << 20);
  CHECK(d->flush().ok());
  Bytes all(N);
  CHECK(d->read_at(0, all.data(), N).ok());
  CHECK(all == model);
  // 独立した検証: nbdcopy で取り出した内容が、こちらの書いた内容と一致する
  if (!e.nbdcopy.empty()) {
    std::string out = e.dir + "/out.img";
    int rc = 0;
    run_capture(e.nbdcopy + " '" + e.uri() + "' " + out + " 2>&1", &rc);
    CHECK_EQ(rc, 0);
    CHECK(read_whole(out) == model);
    // 逆向き: nbdcopy で書いた内容を、こちらが読める
    Bytes inb(N);
    for (auto& b : inb) b = static_cast<uint8_t>(rng());
    for (size_t i = 5u << 20; i < (7u << 20); i++) inb[i] = 0;
    { std::ofstream f(e.dir + "/in.img", std::ios::binary); f.write(reinterpret_cast<const char*>(inb.data()), static_cast<std::streamsize>(inb.size())); }
    run_capture(e.nbdcopy + " " + e.dir + "/in.img '" + e.uri() + "' 2>&1", &rc);
    CHECK_EQ(rc, 0);
    CHECK(d->read_at(0, all.data(), N).ok());
    CHECK(all == inb);
  }
  if (!e.nbdinfo.empty()) {
    std::string sz = run_capture(e.nbdinfo + " --size '" + e.uri() + "'");
    CHECK_EQ(std::strtoull(sz.c_str(), nullptr, 10), N);
  }
}

TEST(fixture_nbd_nbdkit_pattern_64bit_offsets) {
  Ext e("pat");
  if (!have(e.nbdkit, "nbdkit")) return;
  const uint64_t SZ = 4ULL << 40;   // 4 TiB
  Proc p({e.nbdkit, "-f", "-r", "-U", e.sock(), "pattern", std::to_string(SZ)}, e.dir + "/nbdkit.log");
  CHECK(wait_path(e.sock()));
  std::shared_ptr<NbdDevice> d;
  CHECK(NbdDevice::connect(e.opts(), &d).ok());
  if (!d) return;
  CHECK_EQ(d->size(), SZ);
  // pattern プラグイン: 8 バイトごとに、そのブロックの先頭オフセットをビッグエンディアンで並べる
  auto expect = [](uint64_t off, size_t len) {
    Bytes b(len);
    for (size_t i = 0; i < len; i++) {
      uint64_t p = off + i, blk = p & ~7ULL;
      b[i] = static_cast<uint8_t>(blk >> (56 - 8 * (p & 7)));
    }
    return b;
  };
  for (uint64_t off : std::vector<uint64_t>{0, 3, (5ULL << 30) + 3, (1ULL << 40) + 777, SZ - 4096, (1ULL << 32) - 5}) {
    size_t len = 4096;
    if (off + len > SZ) len = static_cast<size_t>(SZ - off);
    Bytes got(len);
    CHECK(d->read_at(off, got.data(), len).ok());
    CHECK(got == expect(off, len));
  }
  Bytes b(1);
  CHECK(d->read_at(SZ, b.data(), 1).code == Errc::OutOfRange);
}

TEST(fixture_nbd_nbdkit_filters) {
  Ext e("flt");
  if (!have(e.nbdkit, "nbdkit")) return;
  // (a) error フィルタ: トリガファイルがあるあいだ EIO。エラー応答のあとも接続は生きている
  {
    std::string trig = e.dir + "/trigger";
    Proc p({e.nbdkit, "-f", "-U", e.sock(), "--filter=error", "memory", "4M", "error=EIO", "error-rate=100%", "error-file=" + trig}, e.dir + "/a.log");
    CHECK(wait_path(e.sock()));
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(e.opts(), &d).ok());
    if (d) {
      Bytes b(4096);
      CHECK(d->read_at(0, b.data(), b.size()).ok());
      { std::ofstream t(trig); t << "x"; }
      Status st = d->read_at(0, b.data(), b.size());
      CHECK(st.code == Errc::Io);
      CHECK_EQ(st.sys_errno, 5);
      st = d->write_at(0, b.data(), b.size());
      CHECK(st.code == Errc::Io);
      ::unlink(trig.c_str());
      CHECK(d->read_at(0, b.data(), b.size()).ok());
      CHECK(d->write_at(0, b.data(), b.size()).ok());
      CHECK_EQ(d->reconnect_count(), 0u);
    }
    p.stop();
    ::unlink(e.sock().c_str());
  }
  // (b) delay フィルタ: 読み取りが遅い → Timeout。書き込みは遅くない
  {
    Proc p({e.nbdkit, "-f", "-U", e.sock(), "--filter=delay", "memory", "4M", "delay-read=700ms"}, e.dir + "/b.log");
    CHECK(wait_path(e.sock()));
    NbdOptions o = e.opts();
    o.io_timeout_ms = 200;
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(o, &d).ok());
    if (d) {
      Bytes b(4096, 3);
      CHECK(d->write_at(0, b.data(), b.size()).ok());
      CHECK(d->read_at(0, b.data(), b.size()).code == Errc::Timeout);
      CHECK(d->read_at(0, b.data(), b.size()).code == Errc::Timeout);
      CHECK(d->reconnect_count() >= 1u);
      IoOptions io;
      io.timeout_ms = 3000;
      CHECK(d->read_at(0, b.data(), b.size(), io).ok());          // 十分待てば読める
      CHECK(b[0] == 3 && b[4095] == 3);                           // 書いた内容 (別接続でも共有)
    }
    p.stop();
    ::unlink(e.sock().c_str());
  }
  // (c) blocksize-policy (違反は EINVAL にする) + log: クライアントが最小/最大を守り、全書き込みに FUA を付ける
  {
    std::string lg = e.dir + "/c.nbdlog";
    Proc p({e.nbdkit, "-f", "-U", e.sock(), "--filter=log", "--filter=blocksize-policy", "memory", "8M", "logfile=" + lg,
            "blocksize-minimum=4096", "blocksize-preferred=65536", "blocksize-maximum=131072", "blocksize-error-policy=error"}, e.dir + "/c.log");
    CHECK(wait_path(e.sock()));
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(e.opts(), &d).ok());
    if (d) {
      CHECK_EQ(d->min_block_size(), 4096u);
      CHECK_EQ(d->max_block_size(), 131072u);
      Bytes model(8u << 20, 0), buf;
      std::mt19937_64 rng(5);
      int errs = 0;
      for (int i = 0; i < 150; i++) {
        size_t len = 1 + rng() % 500000;
        uint64_t off = rng() % (model.size() - len + 1);
        buf.resize(len);
        if (i % 2) {
          for (auto& b : buf) b = static_cast<uint8_t>(rng());
          if (!d->write_at(off, buf.data(), len).ok()) errs++;
          std::memcpy(model.data() + off, buf.data(), len);
        } else {
          if (!d->read_at(off, buf.data(), len).ok() || std::memcmp(buf.data(), model.data() + off, len) != 0) errs++;
        }
      }
      CHECK_EQ(errs, 0);
      CHECK(d->flush().ok());
      d->close();
      p.stop();
      // ログの検査: Read/Write の offset/count が 4096 の倍数で count <= 128K。Write は fua=1
      std::ifstream f(lg);
      std::string line;
      int reads = 0, writes = 0, bad = 0;
      while (std::getline(f, line)) {
        bool is_w = line.find(" Write id=") != std::string::npos && line.find("...Write") == std::string::npos;
        bool is_r = line.find(" Read id=") != std::string::npos && line.find("...Read") == std::string::npos;
        if (!is_w && !is_r) continue;
        size_t po = line.find("offset=0x"), pc = line.find("count=0x");
        if (po == std::string::npos || pc == std::string::npos) continue;
        uint64_t off = std::strtoull(line.c_str() + po + 9, nullptr, 16), cnt = std::strtoull(line.c_str() + pc + 8, nullptr, 16);
        if (off % 4096 || cnt % 4096 || cnt > 131072 || cnt == 0) bad++;
        if (is_w) { writes++; if (line.find("fua=1") == std::string::npos) bad++; } else reads++;
      }
      std::fprintf(stderr, "  nbdkit log: %d reads, %d writes seen, %d violations\n", reads, writes, bad);
      CHECK(reads > 20 && writes > 20);
      CHECK_EQ(bad, 0);
    }
    p.stop();
    ::unlink(e.sock().c_str());
  }
  // (d) 旧式ハンドシェイクのサーバは拒否する
  {
    Proc p({e.nbdkit, "-f", "-o", "-U", e.sock(), "memory", "1M"}, e.dir + "/d.log");
    if (wait_path(e.sock(), 3000)) {
      std::shared_ptr<NbdDevice> d;
      std::string detail;
      Status st = NbdDevice::connect(e.opts(), &d, &detail);
      if (st.ok()) std::fprintf(stderr, "  (note: nbdkit -o still negotiated newstyle)\n");
      else CHECK(st.code == Errc::Unsupported);
    }
  }
}

TEST(fixture_nbd_qemu_nbd_formats_over_nbd) {
  Ext e("qemu");
  if (!have(e.qemu_nbd, "qemu-nbd") || g_fixture_dir.empty()) return;
  Bytes want = read_whole(g_fixture_dir + "/main_mbr.raw");
  if (want.empty()) { std::fprintf(stderr, "  (skip: no fixture)\n"); return; }
  // (a) qemu-nbd が qcow2 を開いて raw として公開 → NBD クライアントで読むと元の RAW
  {
    Proc p({e.qemu_nbd, "-r", "-k", e.sock(), "-f", "qcow2", "-x", "disk", g_fixture_dir + "/q_comp4k.qcow2"}, e.dir + "/q1.log");
    CHECK(wait_path(e.sock()));
    NbdOptions o = e.opts();
    o.export_name = "disk";
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(o, &d).ok());
    if (d) {
      CHECK(d->is_read_only());
      CHECK_EQ(d->size(), want.size());
      Bytes got(want.size());
      CHECK(d->read_at(0, got.data(), got.size()).ok());
      CHECK(got == want);
      d->close();
    }
    // 別名は拒否される
    o.export_name = "other";
    std::string detail;
    Status st = NbdDevice::connect(o, &d, &detail);
    CHECK(!st.ok());
    CHECK(st.code == Errc::InvalidArgument || st.code == Errc::Io);
    // エクスポート一覧
    std::vector<std::string> names;
    NbdOptions lo = e.opts();
    if (NbdDevice::list_exports(lo, &names, &detail).ok()) {
      CHECK(std::find(names.begin(), names.end(), "disk") != names.end());
    }
    p.stop();
    ::unlink(e.sock().c_str());
  }
  // (b) qemu-nbd が qcow2 ファイルを「そのまま(raw)」公開 → NBD を下位にして open_disk_image: 形式を判別して展開できる
  {
    Proc p({e.qemu_nbd, "-r", "-k", e.sock(), "-f", "raw", g_fixture_dir + "/q_comp4k.qcow2"}, e.dir + "/q2.log");
    CHECK(wait_path(e.sock()));
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(e.opts(), &d).ok());
    if (d) {
      BlockDevicePtr img;
      ImageInfo info;
      CHECK(open_disk_image(d, ImageOpenOptions{}, &img, &info).ok());
      CHECK(info.format == ImageFormat::Qcow2);
      if (img) {
        Bytes got(want.size());
        CHECK(img->read_at(0, got.data(), got.size()).ok());
        CHECK(got == want);
        PartitionTable t;
        CHECK(read_partition_table(*img, &t).ok());
        CHECK(t.kind == TableKind::Mbr);
        CHECK_EQ(t.partitions.size(), 2u);
        CHECK(t.partitions[0].fs == FsType::Ext2);
      }
    }
    p.stop();
    ::unlink(e.sock().c_str());
  }
  // (c) 書き込み: qemu-nbd(raw)へ書き、flush、サーバ停止後にファイルの中身を確認 / qemu-img compare でも確認
  {
    std::string f = e.dir + "/w.raw";
    { std::ofstream o(f, std::ios::binary); Bytes z(8u << 20, 0); o.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size())); }
    Proc p({e.qemu_nbd, "-k", e.sock(), "-f", "raw", f}, e.dir + "/q3.log");
    CHECK(wait_path(e.sock()));
    std::shared_ptr<NbdDevice> d;
    CHECK(NbdDevice::connect(e.opts(), &d).ok());
    Bytes model(8u << 20, 0);
    if (d) {
      CHECK(!d->is_read_only());
      std::mt19937_64 rng(9);
      Bytes buf;
      for (int i = 0; i < 80; i++) {
        size_t len = 1 + rng() % 200000;
        uint64_t off = rng() % (model.size() - len + 1);
        buf.resize(len);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        CHECK(d->write_at(off, buf.data(), len).ok());
        std::memcpy(model.data() + off, buf.data(), len);
      }
      CHECK(d->write_zeroes(100000, 500000).ok());
      std::memset(model.data() + 100000, 0, 500000);
      CHECK(d->flush().ok());
      d->close();
    }
    p.stop();
    CHECK(read_whole(f) == model);
  }
}

// ---------------------------------------------------------------- C API
#include "usbmem/c_api_blockdev.h"
TEST(nbd_c_api) {
  FakeNbd s(1u << 20);
  usbmem_device* d = nullptr;
  char* detail = nullptr;
  CHECK_EQ(usbmem_nbd_open("127.0.0.1", s.port, nullptr, "", 0, 2000, 2000, &d, &detail), 0);
  CHECK(d != nullptr);
  if (d) {
    char* js = nullptr;
    CHECK_EQ(usbmem_nbd_info_json(d, &js), 0);
    if (js) { CHECK(std::string(js).find("\"structured\":true") != std::string::npos); usbmem_free(js); }
    long long us = 0;
    CHECK_EQ(usbmem_bench_read(d, 0, 1u << 20, 65536, &us, nullptr), 0);   // 既存の C API が NBD の上でそのまま動く
    usbmem_close(d);
  }
  // 失敗時は理由が返る
  usbmem_device* bad = nullptr;
  char* det2 = nullptr;
  CHECK(usbmem_nbd_open("127.0.0.1", 1, nullptr, "", 0, 300, 300, &bad, &det2) != 0);
  CHECK(det2 != nullptr);
  usbmem_free(det2);
  char* names = nullptr;
  char* det3 = nullptr;
  CHECK_EQ(usbmem_nbd_list_exports_json("127.0.0.1", s.port, nullptr, &names, &det3), 0);
  if (names) { CHECK(std::string(names) == "[\"\",\"second\"]"); usbmem_free(names); }
  CHECK_EQ(usbmem_nbd_open(nullptr, 70000, nullptr, nullptr, 0, 0, 0, &bad, nullptr), static_cast<int>(Errc::InvalidArgument));
  // NBD でないデバイスの info は Unsupported
  usbmem_device* f = nullptr;
  std::string tmp = test_tmpdir() + "/usbmem_capi_nbd_probe";
  { FILE* fp = std::fopen(tmp.c_str(), "wb"); std::fputs("x", fp); std::fclose(fp); }
  CHECK_EQ(usbmem_open_path(tmp.c_str(), 1, &f), 0);
  char* js2 = nullptr;
  CHECK_EQ(usbmem_nbd_info_json(f, &js2), static_cast<int>(Errc::Unsupported));
  usbmem_close(f);
  ::unlink(tmp.c_str());
}
