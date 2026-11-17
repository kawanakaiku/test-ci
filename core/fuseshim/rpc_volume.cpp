// SPDX-License-Identifier: GPL-2.0-or-later
#include "rpc_volume.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace usbmem {

namespace {
enum Op : uint32_t { kLookup = 1, kStat, kReaddir, kRead, kReadlink, kStats };
struct Req { uint32_t op, name_len; uint64_t a, b, c; };
struct Rep { int64_t rc; uint32_t payload_len, pad; };

bool wr_all(int fd, const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n) {
    ssize_t w = ::write(fd, c, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return false;
    c += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}
bool rd_all(int fd, void* p, size_t n) {
  char* c = static_cast<char*>(p);
  while (n) {
    ssize_t r = ::read(fd, c, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    c += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}
}  // namespace

void rpc_serve(int fd, FsVolume& v, const std::function<void(uint64_t*, uint64_t*)>& stats) {
  std::vector<uint8_t> buf, out;
  for (;;) {
    Req q;
    if (!rd_all(fd, &q, sizeof q)) return;
    std::string name(q.name_len, '\0');
    if (q.name_len && !rd_all(fd, &name[0], q.name_len)) return;
    Rep r{0, 0, 0};
    out.clear();
    switch (q.op) {
      case kLookup: { uint64_t ino = 0; r.rc = v.lookup(q.a, name, &ino); if (r.rc == 0) { out.resize(8); std::memcpy(out.data(), &ino, 8); } break; }
      case kStat: { FsStat st; r.rc = v.stat(q.a, &st); if (r.rc == 0) { out.resize(sizeof st); std::memcpy(out.data(), &st, sizeof st); } break; }
      case kReaddir: {
        std::vector<FsDirEntry> ents;
        r.rc = v.readdir(q.a, &ents);
        if (r.rc == 0)
          for (auto& e : ents) {
            uint32_t dt = e.dtype, nl = static_cast<uint32_t>(e.name.size());
            size_t o = out.size();
            out.resize(o + 16 + nl);
            std::memcpy(&out[o], &e.ino, 8);
            std::memcpy(&out[o + 8], &dt, 4);
            std::memcpy(&out[o + 12], &nl, 4);
            std::memcpy(&out[o + 16], e.name.data(), nl);
          }
        break;
      }
      case kRead: {
        buf.resize(static_cast<size_t>(q.c));
        r.rc = v.read(q.a, q.b, buf.data(), buf.size());
        if (r.rc > 0) { r.payload_len = static_cast<uint32_t>(r.rc); }
        break;
      }
      case kReadlink: { std::string t; r.rc = v.readlink(q.a, &t); if (r.rc == 0) out.assign(t.begin(), t.end()); break; }
      case kStats: { uint64_t s[2] = {0, 0}; if (stats) stats(&s[0], &s[1]); out.resize(16); std::memcpy(out.data(), s, 16); break; }
      default: r.rc = -ENOSYS;
    }
    if (q.op != kRead) r.payload_len = static_cast<uint32_t>(out.size());
    if (!wr_all(fd, &r, sizeof r)) return;
    if (q.op == kRead) { if (r.payload_len && !wr_all(fd, buf.data(), r.payload_len)) return; }
    else if (!out.empty() && !wr_all(fd, out.data(), out.size())) return;
  }
}

RpcVolume::~RpcVolume() {
  ::close(fd_);  // 子は EOF で rpc_serve を抜けて後始末 (ntfs_umount) をして終わる
  if (pid_ > 0) { int st; waitpid(pid_, &st, 0); }
}

int64_t RpcVolume::call(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const std::string& name, std::vector<uint8_t>* payload, void* direct, size_t direct_cap) {
  Req q{op, static_cast<uint32_t>(name.size()), a, b, c};
  if (!wr_all(fd_, &q, sizeof q) || (!name.empty() && !wr_all(fd_, name.data(), name.size()))) return -EPIPE;
  Rep r;
  if (!rd_all(fd_, &r, sizeof r)) return -EPIPE;
  if (r.payload_len) {
    if (direct) {  // READ: 呼び出し側のバッファへ直接受ける (本体側の余計なコピーを 1 回減らす)
      if (r.payload_len > direct_cap) return -EIO;
      if (!rd_all(fd_, direct, r.payload_len)) return -EPIPE;
    } else if (payload) {
      payload->resize(r.payload_len);
      if (!rd_all(fd_, payload->data(), r.payload_len)) return -EPIPE;
    }
  }
  return r.rc;
}

int RpcVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  std::vector<uint8_t> p;
  int64_t rc = call(kLookup, dir, 0, 0, name, &p);
  if (rc == 0 && p.size() == 8) std::memcpy(ino, p.data(), 8);
  return static_cast<int>(rc);
}
int RpcVolume::stat(uint64_t ino, FsStat* st) {
  std::vector<uint8_t> p;
  int64_t rc = call(kStat, ino, 0, 0, "", &p);
  if (rc == 0 && p.size() == sizeof *st) std::memcpy(st, p.data(), sizeof *st);
  return static_cast<int>(rc);
}
int RpcVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  std::vector<uint8_t> p;
  int64_t rc = call(kReaddir, dir, 0, 0, "", &p);
  out->clear();
  if (rc != 0) return static_cast<int>(rc);
  for (size_t o = 0; o + 16 <= p.size();) {
    FsDirEntry e;
    uint32_t dt, nl;
    std::memcpy(&e.ino, &p[o], 8);
    std::memcpy(&dt, &p[o + 8], 4);
    std::memcpy(&nl, &p[o + 12], 4);
    if (o + 16 + nl > p.size()) break;
    e.dtype = dt;
    e.name.assign(reinterpret_cast<const char*>(&p[o + 16]), nl);
    out->push_back(std::move(e));
    o += 16 + nl;
  }
  return 0;
}
int64_t RpcVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  std::vector<uint8_t> unused;
  return call(kRead, ino, off, len, "", nullptr, buf, len);
}
int RpcVolume::readlink(uint64_t ino, std::string* out) {
  std::vector<uint8_t> p;
  int64_t rc = call(kReadlink, ino, 0, 0, "", &p);
  if (rc == 0) out->assign(p.begin(), p.end());
  return static_cast<int>(rc);
}
void RpcVolume::dev_stats(uint64_t* calls, uint64_t* bytes) {
  std::vector<uint8_t> p;
  *calls = *bytes = 0;
  if (call(kStats, 0, 0, 0, "", &p) == 0 && p.size() == 16) { std::memcpy(calls, p.data(), 8); std::memcpy(bytes, p.data() + 8, 8); }
}

}  // namespace usbmem
