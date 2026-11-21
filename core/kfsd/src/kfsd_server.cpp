// SPDX-License-Identifier: Apache-2.0
#include "usbmem/kfsd_server.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "kfsd_io.h"

namespace usbmem {
namespace kfsd {

namespace {

// 本体のブロックデバイスを callback で読み書きする BlockDevice。kfsd のドライバはこれの上に載る。ドライバは単一スレッドで呼ぶ前提 (LinuxCompat は kthread を持たない)。
// それでも、全体を mutex で直列化する (ソケット上で同時に進む要求は 1 つ、という契約を守るため)。
class RemoteBlockDevice final : public BlockDevice {
 public:
  RemoteBlockDevice(int fd, uint64_t size, uint32_t bs, bool ro) : fd_(fd), size_(size), bs_(bs ? bs : 512), ro_(ro) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& = {}) override {
    if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
    auto* p = static_cast<uint8_t*>(buf);
    while (len) {
      size_t n = std::min<size_t>(len, kMaxIo);
      Status st = rpc(kDevRead, off, n, nullptr, 0, p, n);
      if (!st.ok()) return st;
      p += n; off += n; len -= n;
    }
    return Status::Ok();
  }
  Status write_at(uint64_t off, const void* buf, size_t len, const IoOptions& = {}) override {
    if (ro_) return Status::Make(Errc::ReadOnly);
    if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
    auto* p = static_cast<const uint8_t*>(buf);
    while (len) {
      size_t n = std::min<size_t>(len, kMaxIo);
      Status st = rpc(kDevWrite, off, 0, p, n, nullptr, 0);
      if (!st.ok()) return st;
      p += n; off += n; len -= n;
    }
    return Status::Ok();
  }
  Status flush() override { return ro_ ? Status::Ok() : rpc(kDevFlush, 0, 0, nullptr, 0, nullptr, 0); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return bs_; }
  bool is_read_only() const override { return ro_; }
  std::string describe() const override { return "kfsd-remote"; }

  bool broken() const { return broken_.load(); }
  uint64_t rc_ = 0, rb_ = 0, wc_ = 0, wb_ = 0;   // 統計 (kStats)

 private:
  // callback を 1 つ送って Reply を待つ。out があれば、Reply の payload (ちょうど out_len バイト) をそこへ受ける。
  Status rpc(uint32_t op, uint64_t a, uint64_t b, const void* pl, size_t plen, void* out, size_t out_len) {
    std::lock_guard<std::mutex> g(mu_);
    if (broken_.load()) return Status::Make(Errc::Io, EPIPE);
    Hdr q{op, static_cast<uint32_t>(plen), ++seq_, a, b, 0};
    if (!io::send_msg(fd_, q, pl, plen, 0)) { broken_ = true; return Status::Make(Errc::Io, EPIPE); }
    Hdr h;
    if (io::recv_hdr(fd_, &h, 0) <= 0 || h.op != kReply || h.seq != q.seq) { broken_ = true; return Status::Make(Errc::Io, EPIPE); }
    int64_t rc = static_cast<int64_t>(h.a);
    std::vector<uint8_t> sink;
    if (rc == 0 && h.len == out_len && out_len) {
      if (io::recv_all(fd_, out, out_len, 0) <= 0) { broken_ = true; return Status::Make(Errc::Io, EPIPE); }
    } else if (h.len) {   // 想定外の payload (エラー応答に付いてきた分など): 読み捨てる。成功なのに長さが違うのはプロトコル違反
      sink.resize(h.len);
      if (io::recv_all(fd_, sink.data(), h.len, 0) <= 0) { broken_ = true; return Status::Make(Errc::Io, EPIPE); }
      if (rc == 0) { broken_ = true; return Status::Make(Errc::Io, EPROTO); }
    } else if (rc == 0 && out_len) {
      broken_ = true;
      return Status::Make(Errc::Io, EPROTO);
    }
    if (op == kDevRead) { rc_++; if (rc == 0) rb_ += b; }
    if (op == kDevWrite) { wc_++; if (rc == 0) wb_ += plen; }
    if (rc == 0) return Status::Ok();
    int e = static_cast<int>(-rc);
    if (e <= 0 || e > 4095) e = EIO;
    return Status::Make(Errc::Io, e);
  }
  int fd_;
  uint64_t size_;
  uint32_t bs_;
  bool ro_;
  std::mutex mu_;
  std::atomic<bool> broken_{false};
  uint64_t seq_ = 0;
};

void put_str(char* dst, size_t cap, const std::string& s) {
  std::memset(dst, 0, cap);
  std::memcpy(dst, s.data(), std::min(s.size(), cap - 1));
}

void fill_info(InfoRep* i, FsVolume& v) {
  std::memset(i, 0, sizeof *i);
  i->writable = v.writable() ? 1 : 0;
  i->state = v.state();
  i->root = v.root();
  put_str(i->fs_name, sizeof i->fs_name, v.fs_name() ? v.fs_name() : "");
}

struct Server {
  Server(int f, const MountFn& m) : fd(f), mount(m) {}
  int fd;
  const MountFn& mount;
  std::shared_ptr<RemoteBlockDevice> dev;
  std::unique_ptr<FsVolume> vol;
  bool hello = false;
  uint64_t requests = 0;
  // readdir のページング: cookie = 次のエントリの添字。最初の要求 (cookie 0) で FsVolume::readdir を呼び、続きはこの一覧から返す
  uint64_t rd_dir = ~0ull;
  std::vector<FsDirEntry> rd_ents;

  bool reply(uint64_t seq, int64_t rc, uint64_t b, uint64_t c, const void* pl, size_t plen) {
    Hdr h{kReply, static_cast<uint32_t>(plen), seq, static_cast<uint64_t>(rc), b, c};
    return io::send_msg(fd, h, pl, plen, 0);
  }
};

// 1 要求を処理する。0 = 続行 / 1 = 通信の失敗 (終了) / 2 = プロトコル違反 (終了) / 100 = kClose (正常終了) / 103 = マウント失敗 (終了)
int handle(Server& s, const Hdr& q, const std::vector<uint8_t>& in) {
  s.requests++;
  auto rep = [&](int64_t rc, uint64_t b = 0, uint64_t c = 0, const void* pl = nullptr, size_t n = 0) { return s.reply(q.seq, rc, b, c, pl, n) ? 0 : 1; };
  auto name_of = [&](std::string* out) { *out = std::string(reinterpret_cast<const char*>(in.data()), in.size()); return valid_name(out->data(), out->size()); };

  if (q.op == kHello) {
    if (s.hello || in.size() != sizeof(HelloMsg)) return 2;
    HelloMsg m;
    std::memcpy(&m, in.data(), sizeof m);
    if (m.magic != kMagic || m.version != kVersion) {
      HelloMsg r{};
      r.magic = kMagic; r.version = kVersion; r.max_payload = kMaxPayload;
      s.reply(q.seq, -EPROTONOSUPPORT, 0, 0, &r, sizeof r);
      return 2;
    }
    HelloMsg r{};
    r.magic = kMagic; r.version = kVersion; r.max_payload = kMaxPayload;
    std::strncpy(r.name, "usbmem-kfsd 1", sizeof r.name - 1);
    s.hello = true;
    return rep(0, 0, 0, &r, sizeof r);
  }
  if (!s.hello) return 2;
  if (q.op == kMount) {
    if (s.vol || in.size() != sizeof(MountReq)) return 2;
    MountReq m;
    std::memcpy(&m, in.data(), sizeof m);
    s.dev = std::make_shared<RemoteBlockDevice>(s.fd, m.dev_size, m.block_size, m.dev_read_only != 0);
    MountParams p;
    p.flags = m.flags;
    p.tz_minutes_west = m.tz_minutes_west;
    MountResult r = s.mount(s.dev, p);
    if (s.dev->broken()) return 1;
    if (!r.vol) {
      int e = r.err > 0 ? r.err : EIO;
      s.reply(q.seq, -e, 0, 0, nullptr, 0);
      return 103;
    }
    s.vol = std::move(r.vol);
    MountRep mr{};
    fill_info(&mr.info, *s.vol);
    put_str(mr.fs_type, sizeof mr.fs_type, r.fs_type);
    return rep(0, 0, 0, &mr, sizeof mr);
  }
  if (!s.vol) return rep(-ENODEV);
  FsVolume& v = *s.vol;
  if (q.op >= kSync && q.op <= kLink) { s.rd_ents.clear(); s.rd_dir = ~0ull; }   // 変更系: readdir の途中の一覧は捨てる
  switch (q.op) {
    case kLookup: {
      std::string n;
      if (!name_of(&n)) return rep(-EINVAL);
      uint64_t ino = 0;
      int rc = v.lookup(q.a, n, &ino);
      return rep(rc, ino);
    }
    case kStat: {
      FsStat st;
      int rc = v.stat(q.a, &st);
      WireStat w{st.ino, st.size, st.mode, st.nlink, st.mtime};
      return rc == 0 ? rep(0, 0, 0, &w, sizeof w) : rep(rc);
    }
    case kReaddir: {
      const bool with_stat = (q.c & kReaddirWithStat) != 0;
      if (q.b == 0 || s.rd_dir != q.a || q.b > s.rd_ents.size()) {
        s.rd_ents.clear();
        s.rd_dir = ~0ull;
        int rc = v.readdir(q.a, &s.rd_ents);
        if (rc != 0) { s.rd_ents.clear(); return rep(rc); }
        s.rd_dir = q.a;
      }
      std::vector<uint8_t> out;
      size_t i = static_cast<size_t>(q.b);
      for (; i < s.rd_ents.size() && out.size() < kReaddirPageBytes; i++) {
        const FsDirEntry& e = s.rd_ents[i];
        if (!valid_name(e.name.data(), e.name.size())) continue;   // 名前として成立しないエントリは出さない
        FsStat st;
        bool has = with_stat && v.stat(e.ino, &st) == 0;
        WireDirentHead hd{e.ino, (e.dtype & 0xFFu) | (has ? kDirentHasStat : 0u), static_cast<uint32_t>(e.name.size())};
        size_t o = out.size();
        out.resize(o + sizeof hd + e.name.size() + (has ? sizeof(WireStat) : 0));
        std::memcpy(&out[o], &hd, sizeof hd);
        std::memcpy(&out[o + sizeof hd], e.name.data(), e.name.size());
        if (has) {
          WireStat w{st.ino, st.size, st.mode, st.nlink, st.mtime};
          std::memcpy(&out[o + sizeof hd + e.name.size()], &w, sizeof w);
        }
      }
      bool more = i < s.rd_ents.size();
      if (!more) { s.rd_ents.clear(); s.rd_dir = ~0ull; }
      return rep(0, i, more ? 1 : 0, out.data(), out.size());
    }
    case kRead: {
      if (q.c > kMaxIo) return rep(-EINVAL);
      std::vector<uint8_t> buf(static_cast<size_t>(q.c));
      int64_t rc = v.read(q.a, q.b, buf.data(), buf.size());
      if (rc > static_cast<int64_t>(buf.size())) rc = -EIO;
      return rc > 0 ? rep(rc, 0, 0, buf.data(), static_cast<size_t>(rc)) : rep(rc);
    }
    case kReadlink: {
      std::string t;
      int rc = v.readlink(q.a, &t);
      return rc == 0 ? rep(0, 0, 0, t.data(), t.size()) : rep(rc);
    }
    case kSync: return rep(v.sync());
    case kCreate: case kMkdir: {
      std::string n;
      if (!name_of(&n)) return rep(-EINVAL);
      uint64_t ino = 0;
      int rc = q.op == kCreate ? v.create(q.a, n, static_cast<uint32_t>(q.b), &ino) : v.mkdir(q.a, n, static_cast<uint32_t>(q.b), &ino);
      return rep(rc, ino);
    }
    case kUnlink: case kRmdir: {
      std::string n;
      if (!name_of(&n)) return rep(-EINVAL);
      return rep(q.op == kUnlink ? v.unlink(q.a, n) : v.rmdir(q.a, n));
    }
    case kRename: {
      if (in.size() < 8) return rep(-EINVAL);
      uint32_t l1, l2;
      std::memcpy(&l1, &in[0], 4);
      std::memcpy(&l2, &in[4], 4);
      if (static_cast<uint64_t>(l1) + l2 + 8 != in.size()) return rep(-EINVAL);
      std::string a(reinterpret_cast<const char*>(&in[8]), l1), b(reinterpret_cast<const char*>(&in[8]) + l1, l2);
      if (!valid_name(a.data(), a.size()) || !valid_name(b.data(), b.size())) return rep(-EINVAL);
      return rep(v.rename(q.a, a, q.b, b));
    }
    case kWrite: case kWriteInplace: {
      if (in.size() > kMaxIo) return rep(-EINVAL);
      int64_t rc = q.op == kWrite ? v.write(q.a, q.b, in.data(), in.size()) : v.write_inplace(q.a, q.b, in.data(), in.size());
      return rep(rc);
    }
    case kTruncate: return rep(v.truncate(q.a, q.b));
    case kSymlink: {
      if (in.size() < 4) return rep(-EINVAL);
      uint32_t l;
      std::memcpy(&l, &in[0], 4);
      if (l > in.size() - 4) return rep(-EINVAL);
      std::string n(reinterpret_cast<const char*>(&in[4]), l), t(reinterpret_cast<const char*>(&in[4]) + l, in.size() - 4 - l);
      if (!valid_name(n.data(), n.size()) || t.empty()) return rep(-EINVAL);
      uint64_t ino = 0;
      int rc = v.symlink(q.a, n, t, &ino);
      return rep(rc, ino);
    }
    case kLink: {
      std::string n;
      if (!name_of(&n)) return rep(-EINVAL);
      return rep(v.link(q.a, n, q.b));
    }
    case kInfo: {
      InfoRep i;
      fill_info(&i, v);
      return rep(0, 0, 0, &i, sizeof i);
    }
    case kStats: {
      StatsRep r{s.dev->rc_, s.dev->rb_, s.dev->wc_, s.dev->wb_, s.requests};
      return rep(0, 0, 0, &r, sizeof r);
    }
    case kClose: {
      s.vol.reset();   // FsVolume のデストラクタ = アンマウント (NTFS はダーティフラグを下ろす、ext4 はジャーナルを閉じる 等)
      return s.reply(q.seq, s.dev->broken() ? -EIO : 0, 0, 0, nullptr, 0) ? 100 : 1;
    }
    default: return rep(-ENOSYS);
  }
}

}  // namespace

int serve(int fd, const MountFn& mount) {
  Server s(fd, mount);
  std::vector<uint8_t> in;
  for (;;) {
    Hdr q;
    int r = io::recv_hdr(fd, &q, 0);
    if (r == 0) break;                       // 本体が閉じた
    if (r < 0) return errno == EPROTO ? 2 : 1;
    in.resize(q.len);
    if (q.len) {
      int rr = io::recv_all(fd, in.data(), q.len, 0);
      if (rr <= 0) return 1;
    }
    int rc;
    try {
      rc = handle(s, q, in);
    } catch (const std::bad_alloc&) {
      rc = s.reply(q.seq, -ENOMEM, 0, 0, nullptr, 0) ? 0 : 1;
    } catch (...) {
      rc = s.reply(q.seq, -EIO, 0, 0, nullptr, 0) ? 0 : 1;
    }
    if (s.dev && s.dev->broken()) return 1;
    if (rc == 100) return 0;
    if (rc == 103) return 3;
    if (rc != 0) return rc;
  }
  // EOF: 本体が (kClose なしに) いなくなった。デバイスはもう無いので、アンマウントの書き込みは失敗する (電源断・抜去と同じ扱い)
  s.vol.reset();
  return 1;
}

}  // namespace kfsd
}  // namespace usbmem
