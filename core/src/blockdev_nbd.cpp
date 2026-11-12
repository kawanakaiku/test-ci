// SPDX-License-Identifier: Apache-2.0
// NBD クライアント (fixed newstyle)。プロトコル: NetworkBlockDevice/nbd doc/proto.md
// 1 接続・同時 1 要求 (内部ミューテックスで直列化)。I/O はすべて poll でタイムアウト/キャンセルを監視する。
// タイムアウト・キャンセルで応答の途中を捨てた接続は同期が壊れているので閉じる (次の I/O で auto_reconnect なら張り直す)。
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <mutex>

#include "blockdev_internal.h"

namespace usbmem {
namespace {

using Clock = std::chrono::steady_clock;
using namespace imgint;

constexpr uint64_t kNbdMagic = 0x4e42444d41474943ULL;     // "NBDMAGIC"
constexpr uint64_t kIHaveOpt = 0x49484156454F5054ULL;     // "IHAVEOPT"
constexpr uint64_t kOptReplyMagic = 0x3e889045565a9ULL;
constexpr uint32_t kReqMagic = 0x25609513u, kSimpleMagic = 0x67446698u, kStructMagic = 0x668e33efu;

enum : uint32_t { OPT_EXPORT_NAME = 1, OPT_ABORT = 2, OPT_LIST = 3, OPT_STRUCTURED_REPLY = 8, OPT_GO = 7 };
enum : uint32_t { REP_ACK = 1, REP_SERVER = 2, REP_INFO = 3, REP_ERR_BIT = 0x80000000u,
                  REP_ERR_UNSUP = 0x80000001u, REP_ERR_POLICY = 0x80000002u, REP_ERR_INVALID = 0x80000003u,
                  REP_ERR_PLATFORM = 0x80000004u, REP_ERR_TLS_REQD = 0x80000005u, REP_ERR_UNKNOWN = 0x80000006u,
                  REP_ERR_SHUTDOWN = 0x80000007u, REP_ERR_BLOCK_SIZE_REQD = 0x80000008u, REP_ERR_TOO_BIG = 0x80000009u };
enum : uint16_t { CMD_READ = 0, CMD_WRITE = 1, CMD_DISC = 2, CMD_FLUSH = 3, CMD_TRIM = 4, CMD_WRITE_ZEROES = 6 };
enum : uint16_t { CF_FUA = 1 };
enum : uint16_t { TF_HAS_FLAGS = 1, TF_READ_ONLY = 2, TF_SEND_FLUSH = 4, TF_SEND_FUA = 8, TF_SEND_TRIM = 32, TF_SEND_WRITE_ZEROES = 64 };
enum : uint16_t { SR_NONE = 0, SR_OFFSET_DATA = 1, SR_OFFSET_HOLE = 2, SR_ERROR = 0x8001, SR_ERROR_OFFSET = 0x8002 };

void p16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
void p32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void p64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i)); }

Status map_nbd_errno(uint32_t e) {
  switch (e) {
    case 1: return Status::Make(Errc::ReadOnly);          // EPERM
    case 5: return Status::Make(Errc::Io, 5);             // EIO
    case 12: return Status::Make(Errc::NoMemory);         // ENOMEM
    case 22: return Status::Make(Errc::InvalidArgument);  // EINVAL
    case 28: return Status::Make(Errc::Io, 28);           // ENOSPC
    case 75: return Status::Make(Errc::OutOfRange);       // EOVERFLOW
    case 95: return Status::Make(Errc::Unsupported);      // ENOTSUP
    case 108: return Status::Make(Errc::Disconnected);    // ESHUTDOWN
    default: return Status::Make(Errc::Io, static_cast<int>(e));
  }
}

struct Deadline {
  Clock::time_point end;
  const CancelToken* cancel = nullptr;
};

Deadline make_deadline(uint32_t ms, const CancelToken* c) {
  return Deadline{Clock::now() + std::chrono::milliseconds(ms), c};
}

// fd が events になるまで待つ。
Status wait_fd(int fd, short events, const Deadline& d) {
  for (;;) {
    if (d.cancel && d.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(d.end - Clock::now()).count();
    if (left <= 0) return Status::Make(Errc::Timeout);
    struct pollfd pf{fd, events, 0};
    int r = ::poll(&pf, 1, static_cast<int>(std::min<long long>(left, 100)));
    if (r < 0) { if (errno == EINTR) continue; return Status::Make(Errc::Io, errno); }
    if (r > 0) {
      if (pf.revents & (events | POLLERR | POLLHUP)) return Status::Ok();
    }
  }
}

class NbdImpl final : public NbdDevice {
 public:
  explicit NbdImpl(const NbdOptions& o) : o_(o) {}
  ~NbdImpl() override { close(); }

  // ---- BlockDevice
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& io = {}) override;
  Status write_at(uint64_t off, const void* buf, size_t len, const IoOptions& io = {}) override;
  Status flush() override;
  Status close() override;
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return ro_; }
  std::string describe() const override { return "nbd(" + (o_.unix_socket.empty() ? o_.host + ":" + std::to_string(o_.port) : o_.unix_socket) + ")"; }
  // ---- NbdDevice
  Status trim(uint64_t off, uint64_t len, const IoOptions& io = {}) override;
  Status write_zeroes(uint64_t off, uint64_t len, const IoOptions& io = {}) override;
  uint32_t min_block_size() const override { return minb_; }
  uint32_t max_block_size() const override { return maxb_; }
  uint16_t transmission_flags() const override { return tflags_; }
  bool structured_replies() const override { return structured_; }
  uint32_t reconnect_count() const override { std::lock_guard<std::mutex> lk(mu_); return reconnects_; }

  // ---- 接続 (static connect / list_exports から使う)
  Status open_socket(const Deadline& dl, std::string* detail);
  Status negotiate(const Deadline& dl, std::string* detail);
  Status list(const Deadline& dl, std::vector<std::string>* names, std::string* detail);
  void drop() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

 private:
  Status send_all(const void* p, size_t n, const Deadline& dl);
  Status recv_all(void* p, size_t n, const Deadline& dl);
  Status skip(size_t n, const Deadline& dl);
  Status greeting(const Deadline& dl, bool* no_zeroes, std::string* detail);
  Status send_opt(uint32_t opt, const void* data, size_t n, const Deadline& dl);
  Status read_opt_reply(uint32_t opt, uint32_t* type, std::vector<uint8_t>* payload, const Deadline& dl);
  Status reconnect(const IoOptions& io);
  uint32_t io_ms(const IoOptions& io) const { return io.timeout_ms ? io.timeout_ms : o_.io_timeout_ms; }
  uint32_t chunk_max() const {
    uint32_t m = std::min<uint32_t>(o_.max_request_bytes, maxb_);
    if (minb_ > 1) m -= m % minb_;
    return m ? m : minb_;
  }

  // 1 回の要求。接続が壊れたら drop() して Disconnected/Io/Timeout/Cancelled を返す。
  Status request_once(uint16_t type, uint16_t flags, uint64_t off, uint32_t len, const void* wbuf, void* rbuf, const IoOptions& io);
  // 再接続つき (冪等な要求のみ)
  Status request(uint16_t type, uint16_t flags, uint64_t off, uint32_t len, const void* wbuf, void* rbuf, const IoOptions& io);
  Status read_locked(uint64_t off, uint8_t* p, size_t len, const IoOptions& io);
  Status write_locked(uint64_t off, const uint8_t* p, size_t len, const IoOptions& io);
  uint16_t write_flags() const { return (o_.fua_writes && (tflags_ & TF_SEND_FUA)) ? CF_FUA : 0; }

  NbdOptions o_;
  mutable std::mutex mu_;
  int fd_ = -1;
  bool closed_ = false, ro_ = false, structured_ = false;
  uint64_t size_ = 0, handle_ = 0;
  uint16_t tflags_ = 0;
  uint32_t minb_ = 1, maxb_ = 32u << 20, reconnects_ = 0;
  bool dirty_ = false, lost_ = false;
  friend class NbdDevice;
};

Status NbdImpl::send_all(const void* p, size_t n, const Deadline& dl) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  while (n > 0) {
    Status w = wait_fd(fd_, POLLOUT, dl);
    if (!w.ok()) return w;
    ssize_t r = ::send(fd_, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (errno == EPIPE || errno == ECONNRESET) return Status::Make(Errc::Disconnected);
      return Status::Make(Errc::Io, errno);
    }
    b += r; n -= static_cast<size_t>(r);
  }
  return Status::Ok();
}

Status NbdImpl::recv_all(void* p, size_t n, const Deadline& dl) {
  uint8_t* b = static_cast<uint8_t*>(p);
  while (n > 0) {
    Status w = wait_fd(fd_, POLLIN, dl);
    if (!w.ok()) return w;
    ssize_t r = ::recv(fd_, b, n, MSG_DONTWAIT);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (errno == ECONNRESET) return Status::Make(Errc::Disconnected);
      return Status::Make(Errc::Io, errno);
    }
    if (r == 0) return Status::Make(Errc::Disconnected);   // EOF
    b += r; n -= static_cast<size_t>(r);
  }
  return Status::Ok();
}

Status NbdImpl::skip(size_t n, const Deadline& dl) {
  uint8_t tmp[1024];
  while (n > 0) {
    size_t k = std::min(n, sizeof tmp);
    Status st = recv_all(tmp, k, dl);
    if (!st.ok()) return st;
    n -= k;
  }
  return Status::Ok();
}

Status NbdImpl::open_socket(const Deadline& dl, std::string* detail) {
  drop();
  auto try_connect = [&](int family, int type, const sockaddr* sa, socklen_t sl) -> Status {
    int fd = ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return Status::Make(Errc::Io, errno);
    int rc = ::connect(fd, sa, sl);
    if (rc != 0 && errno != EINPROGRESS) { int e = errno; ::close(fd); return Status::Make(Errc::Io, e); }
    if (rc != 0) {
      Status w = wait_fd(fd, POLLOUT, dl);
      if (!w.ok()) { ::close(fd); return w; }
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err != 0) { ::close(fd); return Status::Make(Errc::Io, err); }
    }
    if (family != AF_UNIX) { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    fd_ = fd;
    return Status::Ok();
  };
  if (!o_.unix_socket.empty()) {
    sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (o_.unix_socket.size() >= sizeof sa.sun_path) { if (detail) *detail = "unix socket path too long"; return Status::Make(Errc::InvalidArgument); }
    std::memcpy(sa.sun_path, o_.unix_socket.data(), o_.unix_socket.size());
    socklen_t sl = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + o_.unix_socket.size() + (o_.unix_socket[0] == '@' ? 0 : 1));
    if (o_.unix_socket[0] == '@') sa.sun_path[0] = '\0';   // 抽象名前空間
    Status st = try_connect(AF_UNIX, SOCK_STREAM, reinterpret_cast<sockaddr*>(&sa), sl);
    if (!st.ok() && detail) *detail = "cannot connect to unix socket";
    return st;
  }
  if (o_.host.empty()) { if (detail) *detail = "no host"; return Status::Make(Errc::InvalidArgument); }
  addrinfo hints;
  std::memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  int gai = getaddrinfo(o_.host.c_str(), std::to_string(o_.port).c_str(), &hints, &res);
  if (gai != 0 || !res) { if (detail) *detail = std::string("name resolution failed: ") + gai_strerror(gai); return Status::Make(Errc::Io, 0); }
  Status last = Status::Make(Errc::Io);
  for (addrinfo* a = res; a; a = a->ai_next) {
    last = try_connect(a->ai_family, a->ai_socktype, a->ai_addr, a->ai_addrlen);
    if (last.ok() || last.code == Errc::Timeout || last.code == Errc::Cancelled) break;
  }
  freeaddrinfo(res);
  if (!last.ok() && detail) *detail = "cannot connect to " + o_.host + ":" + std::to_string(o_.port);
  return last;
}

Status NbdImpl::greeting(const Deadline& dl, bool* no_zeroes, std::string* detail) {
  uint8_t g[18];
  Status st = recv_all(g, 16, dl);
  if (!st.ok()) return st;
  if (be64(g) != kNbdMagic) { if (detail) *detail = "not an NBD server (bad magic)"; return Status::Make(Errc::Corrupt); }
  if (be64(g + 8) != kIHaveOpt) {
    if (detail) *detail = "old-style NBD handshake is not supported";
    return Status::Make(Errc::Unsupported);
  }
  st = recv_all(g, 2, dl);
  if (!st.ok()) return st;
  uint16_t hf = be16(g);
  if (!(hf & 1)) { if (detail) *detail = "server does not support fixed newstyle negotiation"; return Status::Make(Errc::Unsupported); }
  *no_zeroes = (hf & 2) != 0;
  uint8_t cf[4];
  p32(cf, 1u | (*no_zeroes ? 2u : 0u));
  return send_all(cf, 4, dl);
}

Status NbdImpl::send_opt(uint32_t opt, const void* data, size_t n, const Deadline& dl) {
  std::vector<uint8_t> m(16 + n);
  p64(m.data(), kIHaveOpt);
  p32(m.data() + 8, opt);
  p32(m.data() + 12, static_cast<uint32_t>(n));
  if (n) std::memcpy(m.data() + 16, data, n);
  return send_all(m.data(), m.size(), dl);
}

Status NbdImpl::read_opt_reply(uint32_t opt, uint32_t* type, std::vector<uint8_t>* payload, const Deadline& dl) {
  uint8_t h[20];
  Status st = recv_all(h, 20, dl);
  if (!st.ok()) return st;
  if (be64(h) != kOptReplyMagic || be32(h + 8) != opt) return Status::Make(Errc::Corrupt);
  *type = be32(h + 12);
  uint32_t len = be32(h + 16);
  if (len > (1u << 20)) return Status::Make(Errc::Corrupt);
  payload->assign(len, 0);
  if (len) { st = recv_all(payload->data(), len, dl); if (!st.ok()) return st; }
  return Status::Ok();
}

Status NbdImpl::negotiate(const Deadline& dl, std::string* detail) {
  bool no_zeroes = false;
  Status st = greeting(dl, &no_zeroes, detail);
  if (!st.ok()) return st;
  uint32_t type;
  std::vector<uint8_t> pl;
  structured_ = false;
  if (o_.use_structured_replies) {
    st = send_opt(OPT_STRUCTURED_REPLY, nullptr, 0, dl);
    if (!st.ok()) return st;
    st = read_opt_reply(OPT_STRUCTURED_REPLY, &type, &pl, dl);
    if (!st.ok()) return st;
    structured_ = (type == REP_ACK);
  }
  // OPT_GO
  std::vector<uint8_t> go(4 + o_.export_name.size() + 2 + 4);
  p32(go.data(), static_cast<uint32_t>(o_.export_name.size()));
  std::memcpy(go.data() + 4, o_.export_name.data(), o_.export_name.size());
  p16(go.data() + 4 + o_.export_name.size(), 2);
  p16(go.data() + 6 + o_.export_name.size(), 0);   // NBD_INFO_EXPORT
  p16(go.data() + 8 + o_.export_name.size(), 3);   // NBD_INFO_BLOCK_SIZE
  st = send_opt(OPT_GO, go.data(), go.size(), dl);
  if (!st.ok()) return st;
  bool got_export = false, unsup = false;
  minb_ = 1; maxb_ = 32u << 20;
  for (;;) {
    st = read_opt_reply(OPT_GO, &type, &pl, dl);
    if (!st.ok()) return st;
    if (type == REP_ACK) break;
    if (type == REP_INFO) {
      if (pl.size() < 2) return Status::Make(Errc::Corrupt);
      uint16_t it = be16(pl.data());
      if (it == 0 && pl.size() >= 12) { size_ = be64(pl.data() + 2); tflags_ = be16(pl.data() + 10); got_export = true; }
      else if (it == 3 && pl.size() >= 14) {
        uint32_t mn = be32(pl.data() + 2), mx = be32(pl.data() + 10);
        if (mn >= 1 && mn <= 65536 && is_pow2(mn)) minb_ = mn;
        if (mx != 0 && mx >= minb_) maxb_ = std::min<uint32_t>(mx, 1u << 30);
      }
      continue;
    }
    if (type & REP_ERR_BIT) {
      std::string msg(pl.begin(), pl.end());
      if (type == REP_ERR_UNSUP) { unsup = true; break; }
      if (detail) {
        switch (type) {
          case REP_ERR_UNKNOWN: *detail = "export not found: '" + o_.export_name + "'"; break;
          case REP_ERR_POLICY: *detail = "server refused (policy)" + (msg.empty() ? "" : ": " + msg); break;
          case REP_ERR_TLS_REQD: *detail = "server requires TLS (not supported)"; break;
          case REP_ERR_BLOCK_SIZE_REQD: *detail = "server requires block size negotiation"; break;
          default: *detail = "server error " + std::to_string(type & 0x7fffffff) + (msg.empty() ? "" : ": " + msg); break;
        }
      }
      if (type == REP_ERR_TLS_REQD) return Status::Make(Errc::Unsupported);
      if (type == REP_ERR_UNKNOWN) return Status::Make(Errc::InvalidArgument);
      return Status::Make(Errc::Io, 0);
    }
    return Status::Make(Errc::Corrupt);
  }
  if (unsup) {   // 古いサーバ: NBD_OPT_EXPORT_NAME
    st = send_opt(OPT_EXPORT_NAME, o_.export_name.data(), o_.export_name.size(), dl);
    if (!st.ok()) return st;
    uint8_t r[10];
    st = recv_all(r, 10, dl);
    if (!st.ok()) { if (detail && st.code == Errc::Disconnected) *detail = "server closed the connection (unknown export?)"; return st; }
    size_ = be64(r);
    tflags_ = be16(r + 8);
    got_export = true;
    if (!no_zeroes) { st = skip(124, dl); if (!st.ok()) return st; }
  }
  if (!got_export) { if (detail) *detail = "server did not report the export size"; return Status::Make(Errc::Corrupt); }
  if (size_ > (1ULL << 62)) { if (detail) *detail = "implausible export size"; return Status::Make(Errc::Corrupt); }
  if (!(tflags_ & TF_HAS_FLAGS)) { if (detail) *detail = "server sent no transmission flags"; return Status::Make(Errc::Corrupt); }
  if (minb_ > 1 && size_ % minb_ != 0) { if (detail) *detail = "export size is not a multiple of the minimum block size"; return Status::Make(Errc::Corrupt); }
  ro_ = o_.read_only || (tflags_ & TF_READ_ONLY);
  return Status::Ok();
}

Status NbdImpl::list(const Deadline& dl, std::vector<std::string>* names, std::string* detail) {
  bool nz = false;
  Status st = greeting(dl, &nz, detail);
  if (!st.ok()) return st;
  st = send_opt(OPT_LIST, nullptr, 0, dl);
  if (!st.ok()) return st;
  for (;;) {
    uint32_t type;
    std::vector<uint8_t> pl;
    st = read_opt_reply(OPT_LIST, &type, &pl, dl);
    if (!st.ok()) return st;
    if (type == REP_ACK) break;
    if (type == REP_SERVER) {
      if (pl.size() < 4) return Status::Make(Errc::Corrupt);
      uint32_t nl = be32(pl.data());
      if (nl > pl.size() - 4) return Status::Make(Errc::Corrupt);
      names->emplace_back(reinterpret_cast<char*>(pl.data() + 4), nl);
      if (names->size() > 100000) return Status::Make(Errc::Corrupt);
      continue;
    }
    if (type & REP_ERR_BIT) {
      if (detail) *detail = type == REP_ERR_POLICY ? "server does not allow listing exports" : "NBD_OPT_LIST failed";
      return Status::Make(type == REP_ERR_UNSUP || type == REP_ERR_POLICY ? Errc::Unsupported : Errc::Io);
    }
    return Status::Make(Errc::Corrupt);
  }
  send_opt(OPT_ABORT, nullptr, 0, dl);   // best effort
  return Status::Ok();
}

Status NbdImpl::request_once(uint16_t type, uint16_t flags, uint64_t off, uint32_t len, const void* wbuf, void* rbuf, const IoOptions& io) {
  if (fd_ < 0) return Status::Make(Errc::Disconnected);
  Deadline dl = make_deadline(io_ms(io), io.cancel);
  uint64_t h = ++handle_;
  uint8_t rq[28];
  p32(rq, kReqMagic);
  p16(rq + 4, flags);
  p16(rq + 6, type);
  p64(rq + 8, h);
  p64(rq + 16, off);
  p32(rq + 24, len);
  Status st = send_all(rq, 28, dl);
  if (st.ok() && type == CMD_WRITE) st = send_all(wbuf, len, dl);
  if (!st.ok()) { drop(); return st; }

  // 応答
  uint64_t covered = 0;
  Status first_err;
  for (;;) {
    uint8_t m[4];
    st = recv_all(m, 4, dl);
    if (!st.ok()) { drop(); return st; }
    uint32_t magic = be32(m);
    if (magic == kSimpleMagic) {
      uint8_t r[12];
      st = recv_all(r, 12, dl);
      if (!st.ok()) { drop(); return st; }
      if (be64(r + 4) != h) { drop(); return Status::Make(Errc::Corrupt); }
      uint32_t err = be32(r);
      if (err) return map_nbd_errno(err);   // READ のエラー応答にデータは続かない
      if (type == CMD_READ) {
        if (structured_) { drop(); return Status::Make(Errc::Corrupt); }
        st = recv_all(rbuf, len, dl);
        if (!st.ok()) { drop(); return st; }
      }
      return Status::Ok();
    }
    if (magic != kStructMagic || !structured_) { drop(); return Status::Make(Errc::Corrupt); }
    uint8_t r[16];
    st = recv_all(r, 16, dl);
    if (!st.ok()) { drop(); return st; }
    uint16_t sflags = be16(r), stype = be16(r + 2);
    uint32_t plen = be32(r + 12);
    if (be64(r + 4) != h || plen > len + 65536u) { drop(); return Status::Make(Errc::Corrupt); }
    if (stype == SR_NONE) {
      if (plen != 0) { drop(); return Status::Make(Errc::Corrupt); }
    } else if (stype == SR_OFFSET_DATA) {
      if (type != CMD_READ || plen < 9) { drop(); return Status::Make(Errc::Corrupt); }
      uint8_t ob[8];
      st = recv_all(ob, 8, dl);
      if (!st.ok()) { drop(); return st; }
      uint64_t o = be64(ob), dlen = plen - 8;
      if (o < off || o - off > len || dlen > len - (o - off)) { drop(); return Status::Make(Errc::Corrupt); }
      st = recv_all(static_cast<uint8_t*>(rbuf) + (o - off), static_cast<size_t>(dlen), dl);
      if (!st.ok()) { drop(); return st; }
      covered += dlen;
    } else if (stype == SR_OFFSET_HOLE) {
      if (type != CMD_READ || plen != 12) { drop(); return Status::Make(Errc::Corrupt); }
      uint8_t hb[12];
      st = recv_all(hb, 12, dl);
      if (!st.ok()) { drop(); return st; }
      uint64_t o = be64(hb), hl = be32(hb + 8);
      if (o < off || o - off > len || hl > len - (o - off)) { drop(); return Status::Make(Errc::Corrupt); }
      std::memset(static_cast<uint8_t*>(rbuf) + (o - off), 0, static_cast<size_t>(hl));
      covered += hl;
    } else if (stype == SR_ERROR || stype == SR_ERROR_OFFSET) {
      if (plen < 6) { drop(); return Status::Make(Errc::Corrupt); }
      uint8_t eb[6];
      st = recv_all(eb, 6, dl);
      if (!st.ok()) { drop(); return st; }
      if (first_err.ok()) first_err = map_nbd_errno(be32(eb));
      st = skip(plen - 6, dl);
      if (!st.ok()) { drop(); return st; }
    } else {
      drop();
      return Status::Make(Errc::Corrupt);   // 未知のチャンクタイプ
    }
    if (sflags & 1) break;   // DONE
  }
  if (!first_err.ok()) return first_err;
  if (type == CMD_READ && covered != len) { drop(); return Status::Make(Errc::Corrupt); }
  return Status::Ok();
}

Status NbdImpl::reconnect(const IoOptions& io) {
  Deadline dl = make_deadline(o_.connect_timeout_ms, io.cancel);
  drop();
  if (dirty_) { lost_ = true; dirty_ = false; }   // 未 flush の書き込みは失われた可能性がある
  uint64_t old_size = size_;
  uint16_t old_ro = tflags_ & TF_READ_ONLY;
  std::string d;
  Status st = open_socket(dl, &d);
  if (st.ok()) st = negotiate(dl, &d);
  if (!st.ok()) { drop(); return st.code == Errc::Cancelled || st.code == Errc::Timeout ? st : Status::Make(Errc::Disconnected); }
  if (size_ != old_size || (tflags_ & TF_READ_ONLY) != old_ro) { drop(); return Status::Make(Errc::Disconnected); }   // 別のエクスポートになっている
  reconnects_++;
  return Status::Ok();
}

Status NbdImpl::request(uint16_t type, uint16_t flags, uint64_t off, uint32_t len, const void* wbuf, void* rbuf, const IoOptions& io) {
  if (fd_ < 0) {
    if (!o_.auto_reconnect) return Status::Make(Errc::Disconnected);
    Status r = reconnect(io);
    if (!r.ok()) return r;
  }
  Status st = request_once(type, flags, off, len, wbuf, rbuf, io);
  if (st.ok() || fd_ >= 0) return st;
  // 接続が壊れた。Disconnected/Io (相手が切った・リセット) だけ 1 回再試行する。Timeout/Cancelled は呼び出し側へ。
  if (!o_.auto_reconnect || (st.code != Errc::Disconnected && st.code != Errc::Io)) return st;
  Status r = reconnect(io);
  if (!r.ok()) return r;
  return request_once(type, flags, off, len, wbuf, rbuf, io);
}

Status NbdImpl::read_locked(uint64_t off, uint8_t* p, size_t len, const IoOptions& io) {
  const uint32_t cm = chunk_max();
  std::vector<uint8_t> tmp;
  while (len > 0) {
    if (io.cancel && io.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t a0 = minb_ > 1 ? off - off % minb_ : off;   // 揃えた範囲が cm (サーバの最大) を超えないように end を決める
    uint64_t end = std::min<uint64_t>(off + len, a0 + cm);
    bool aligned = minb_ <= 1 || ((off % minb_) == 0 && ((end % minb_) == 0 || end == size_));
    if (aligned) {
      Status st = request(CMD_READ, 0, off, static_cast<uint32_t>(end - off), nullptr, p, io);
      if (!st.ok()) return st;
      p += end - off; len -= static_cast<size_t>(end - off); off = end;
    } else {
      uint64_t a = off - off % minb_, b = std::min<uint64_t>(size_, (end + minb_ - 1) / minb_ * minb_);
      tmp.resize(static_cast<size_t>(b - a));
      Status st = request(CMD_READ, 0, a, static_cast<uint32_t>(b - a), nullptr, tmp.data(), io);
      if (!st.ok()) return st;
      std::memcpy(p, tmp.data() + (off - a), static_cast<size_t>(end - off));
      p += end - off; len -= static_cast<size_t>(end - off); off = end;
    }
  }
  return Status::Ok();
}

Status NbdImpl::write_locked(uint64_t off, const uint8_t* p, size_t len, const IoOptions& io) {
  const uint32_t cm = chunk_max();
  std::vector<uint8_t> tmp;
  while (len > 0) {
    if (io.cancel && io.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t a0 = minb_ > 1 ? off - off % minb_ : off;   // 揃えた範囲が cm (サーバの最大) を超えないように end を決める
    uint64_t end = std::min<uint64_t>(off + len, a0 + cm);
    bool aligned = minb_ <= 1 || ((off % minb_) == 0 && ((end % minb_) == 0 || end == size_));
    uint16_t fl = write_flags();
    Status st;
    if (aligned) {
      st = request(CMD_WRITE, fl, off, static_cast<uint32_t>(end - off), p, nullptr, io);
    } else {
      uint64_t a = off - off % minb_, b = std::min<uint64_t>(size_, (end + minb_ - 1) / minb_ * minb_);
      tmp.resize(static_cast<size_t>(b - a));
      st = request(CMD_READ, 0, a, static_cast<uint32_t>(b - a), nullptr, tmp.data(), io);
      if (!st.ok()) return st;
      std::memcpy(tmp.data() + (off - a), p, static_cast<size_t>(end - off));
      st = request(CMD_WRITE, fl, a, static_cast<uint32_t>(b - a), tmp.data(), nullptr, io);
    }
    if (!st.ok()) return st;
    if (!fl) dirty_ = true;
    p += end - off; len -= static_cast<size_t>(end - off); off = end;
  }
  return Status::Ok();
}

Status NbdImpl::read_at(uint64_t off, void* buf, size_t len, const IoOptions& io) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  return read_locked(off, static_cast<uint8_t*>(buf), len, io);
}

Status NbdImpl::write_at(uint64_t off, const void* buf, size_t len, const IoOptions& io) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (ro_) return Status::Make(Errc::ReadOnly);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  return write_locked(off, static_cast<const uint8_t*>(buf), len, io);
}

Status NbdImpl::flush() {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (lost_) { lost_ = false; return Status::Make(Errc::Disconnected); }   // 再接続をまたいだ未 flush 書き込みの喪失を通知
  if (ro_ || !(tflags_ & TF_SEND_FLUSH)) return Status::Ok();
  Status st = request(CMD_FLUSH, 0, 0, 0, nullptr, nullptr, IoOptions{});
  if (!st.ok()) return st;
  if (lost_) { lost_ = false; return Status::Make(Errc::Disconnected); }   // この flush 中の再接続で失われた
  dirty_ = false;
  return Status::Ok();
}

Status NbdImpl::trim(uint64_t off, uint64_t len, const IoOptions& io) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (ro_) return Status::Make(Errc::ReadOnly);
  if (!(tflags_ & TF_SEND_TRIM)) return Status::Make(Errc::Unsupported);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  // TRIM は助言的: 最小ブロックに揃わない端は捨てる
  uint64_t a = minb_ > 1 ? (off + minb_ - 1) / minb_ * minb_ : off, b = minb_ > 1 ? (off + len) / minb_ * minb_ : off + len;
  const uint32_t cm = chunk_max();
  while (a < b) {
    uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(b - a, cm));
    Status st = request(CMD_TRIM, write_flags(), a, n, nullptr, nullptr, io);
    if (!st.ok()) return st;
    a += n;
  }
  return Status::Ok();
}

Status NbdImpl::write_zeroes(uint64_t off, uint64_t len, const IoOptions& io) {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Make(Errc::Closed);
  if (ro_) return Status::Make(Errc::ReadOnly);
  if (!(tflags_ & TF_SEND_WRITE_ZEROES)) return Status::Make(Errc::Unsupported);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  const uint32_t cm = chunk_max();
  std::vector<uint8_t> zeros;
  uint64_t pos = off, end = off + len;
  while (pos < end) {
    uint64_t n = std::min<uint64_t>(end - pos, cm);
    bool aligned = minb_ <= 1 || ((pos % minb_) == 0 && (((pos + n) % minb_) == 0 || pos + n == size_));
    Status st;
    if (aligned) {
      st = request(CMD_WRITE_ZEROES, write_flags(), pos, static_cast<uint32_t>(n), nullptr, nullptr, io);
      if (st.ok() && !write_flags()) dirty_ = true;
    } else {
      zeros.assign(static_cast<size_t>(std::min<uint64_t>(n, 2 * minb_)), 0);
      n = std::min<uint64_t>(n, zeros.size());
      st = write_locked(pos, zeros.data(), static_cast<size_t>(n), io);
    }
    if (!st.ok()) return st;
    pos += n;
  }
  return Status::Ok();
}

Status NbdImpl::close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (closed_) return Status::Ok();
  closed_ = true;
  if (fd_ >= 0) {
    uint8_t rq[28];
    p32(rq, kReqMagic);
    p16(rq + 4, 0);
    p16(rq + 6, CMD_DISC);
    p64(rq + 8, ++handle_);
    p64(rq + 16, 0);
    p32(rq + 24, 0);
    ::send(fd_, rq, 28, MSG_NOSIGNAL | MSG_DONTWAIT);   // best effort
    drop();
  }
  return Status::Ok();
}

}  // namespace

Status NbdDevice::connect(const NbdOptions& opt, std::shared_ptr<NbdDevice>* out, std::string* detail) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  auto dev = std::make_shared<NbdImpl>(opt);
  Deadline dl = make_deadline(opt.connect_timeout_ms, nullptr);
  std::string d;
  Status st = dev->open_socket(dl, &d);
  if (st.ok()) st = dev->negotiate(dl, &d);
  if (!st.ok()) {
    dev->drop();
    if (detail) *detail = d;
    return st;
  }
  *out = std::move(dev);
  return Status::Ok();
}

Status NbdDevice::list_exports(const NbdOptions& opt, std::vector<std::string>* names, std::string* detail) {
  if (!names) return Status::Make(Errc::InvalidArgument);
  NbdImpl dev(opt);
  Deadline dl = make_deadline(opt.connect_timeout_ms, nullptr);
  std::string d;
  Status st = dev.open_socket(dl, &d);
  if (st.ok()) st = dev.list(dl, names, &d);
  dev.drop();
  if (!st.ok() && detail) *detail = d;
  return st;
}

}  // namespace usbmem
