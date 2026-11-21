// SPDX-License-Identifier: Apache-2.0
#include "usbmem/kfsd_client.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "kfsd_io.h"

extern char** environ;

namespace usbmem {
namespace kfsd {

namespace {
std::mutex g_exe_mu;
std::string g_exe;

int status_to_errno(const Status& s) {
  if (s.ok()) return 0;
  if (s.sys_errno > 0) return -s.sys_errno;
  switch (s.code) {
    case Errc::OutOfRange: return -EINVAL;
    case Errc::ReadOnly: return -EROFS;
    case Errc::Disconnected: return -ENODEV;
    case Errc::Timeout: return -ETIMEDOUT;
    case Errc::Closed: return -EBADF;
    case Errc::Cancelled: return -ECANCELED;
    case Errc::NoMemory: return -ENOMEM;
    case Errc::InvalidArgument: return -EINVAL;
    default: return -EIO;
  }
}
void copy_cstr(std::string* out, const char* p, size_t cap) {
  size_t n = 0;
  while (n < cap && p[n]) n++;
  out->assign(p, n);
}
}  // namespace

std::string find_exe() {
  if (const char* e = std::getenv("USBMEM_KFSD")) if (*e) return e;
  std::lock_guard<std::mutex> g(g_exe_mu);
  return g_exe;
}
void set_exe(const std::string& path) {
  std::lock_guard<std::mutex> g(g_exe_mu);
  g_exe = path;
}

int spawn(const std::string& exe, const std::vector<std::string>& extra_args, int* fd, pid_t* pid) {
  if (exe.empty() || ::access(exe.c_str(), X_OK) != 0) return -ENOENT;
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -errno;
  // 子に渡す端は fd 3 へ複製する (dup2 は FD_CLOEXEC を落とす)。同じ番号のときの特例を避けるため、いったん 10 以上へ移す。
  int child = ::fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
  if (child < 0) { int e = errno; ::close(sv[0]); ::close(sv[1]); return -e; }
  ::close(sv[1]);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, child, 3);
  std::vector<std::string> args{exe, "--fd=3"};
  for (auto& a : extra_args) args.push_back(a);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t p = -1;
  int rc = ::posix_spawn(&p, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(child);
  if (rc != 0) { ::close(sv[0]); return -rc; }
  *fd = sv[0];
  *pid = p;
  return 0;
}

RpcVolume::~RpcVolume() { reap(true); }

// 後始末: 生きていれば kClose で正常にアンマウントさせ、fd を閉じ、子を待つ (待ちきれなければ SIGKILL)。
void RpcVolume::reap(bool graceful) {
  if (fd_ >= 0) {
    if (graceful && !dead_.load()) {
      Reply r;
      call(kClose, 0, 0, 0, nullptr, 0, &r, nullptr, nullptr, 0, 30000);
    }
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  dead_.store(true);
  if (pid_ > 0) {
    int st = 0;
    for (int i = 0; i < 250; i++) {   // 最大 5 秒
      pid_t w = ::waitpid(pid_, &st, WNOHANG);
      if (w == pid_) { exit_status_ = st; pid_ = -1; return; }
      if (w < 0 && errno != EINTR) { pid_ = -1; return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid_, SIGKILL);
    if (::waitpid(pid_, &st, 0) == pid_) exit_status_ = st;
    pid_ = -1;
  }
}

void RpcVolume::abort() {
  dead_.store(true);
  // fd_ は mu_ の外から閉じない (別スレッドが使用中かもしれない)。shutdown で待ちを起こし、子を殺す。fd の close はデストラクタ。
  int fd = fd_;
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
  if (pid_ > 0) ::kill(pid_, SIGKILL);
}

int64_t RpcVolume::fail(int err, bool kill) {
  dead_.store(true);
  if (kill) {
    if (pid_ > 0) ::kill(pid_, SIGKILL);
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }
  return err > 0 ? -err : err;
}

// callback (kfsd -> 本体) を処理して Reply を返す。false = 通信の失敗 (呼び出し側が fail する)。
bool RpcVolume::handle_callback(const Hdr& h, int timeout_ms) {
  Hdr rep{kReply, 0, h.seq, 0, 0, 0};
  const void* pl = nullptr;
  size_t plen = 0;
  int64_t rc = 0;
  if (h.op == kDevRead) {
    if (h.len != 0 || h.b > kMaxIo) { errno = EPROTO; return false; }
    devbuf_.resize(static_cast<size_t>(h.b));
    Status st = dev_->read_at(h.a, devbuf_.data(), devbuf_.size());
    rc = status_to_errno(st);
    dev_rc_++;
    if (rc == 0) { dev_rb_ += h.b; pl = devbuf_.data(); plen = devbuf_.size(); }
  } else if (h.op == kDevWrite) {
    if (h.len > kMaxIo) { errno = EPROTO; return false; }
    devbuf_.resize(h.len);
    if (h.len) {
      int r = io::recv_all(fd_, devbuf_.data(), h.len, timeout_ms);
      if (r <= 0) { if (r == 0) errno = EPIPE; return false; }
    }
    Status st = dev_->write_at(h.a, devbuf_.data(), devbuf_.size());
    rc = status_to_errno(st);
    dev_wc_++;
    if (rc == 0) dev_wb_ += h.len;
  } else {  // kDevFlush
    if (h.len != 0) { errno = EPROTO; return false; }
    rc = status_to_errno(dev_->flush());
  }
  rep.a = static_cast<uint64_t>(rc);
  rep.len = static_cast<uint32_t>(plen);
  return io::send_msg(fd_, rep, pl, plen, timeout_ms);
}

int64_t RpcVolume::call(uint32_t op, uint64_t a, uint64_t b, uint64_t c, const void* pl, size_t plen, Reply* rep, std::vector<uint8_t>* out,
                        void* direct, size_t direct_cap, int timeout_ms) {
  std::lock_guard<std::mutex> g(mu_);
  if (dead_.load() || fd_ < 0) return -EPIPE;
  if (timeout_ms < 0) timeout_ms = opt_.timeout_ms;
  if (plen > kMaxPayload) return -EMSGSIZE;
  if (op >= kSync && op <= kLink) { gen_++; stat_cache_.clear(); }   // 変更系 (stat キャッシュを捨てる)
  Hdr q{op, static_cast<uint32_t>(plen), ++seq_, a, b, c};
  if (!io::send_msg(fd_, q, pl, plen, timeout_ms)) { int e = errno; return fail(e == ETIMEDOUT ? ETIMEDOUT : EPIPE, true); }
  for (;;) {
    Hdr h;
    int r = io::recv_hdr(fd_, &h, timeout_ms);
    if (r == 0) return fail(EPIPE, false);                       // kfsd が閉じた / 死んだ
    if (r < 0) return fail(errno == ETIMEDOUT ? ETIMEDOUT : errno == EPROTO ? EPROTO : EPIPE, true);
    if (h.op == kReply) {
      if (h.seq != q.seq) return fail(EPROTO, true);
      rep->rc = static_cast<int64_t>(h.a);
      rep->b = h.b;
      rep->c = h.c;
      rep->len = h.len;
      if (h.len) {
        int rr;
        if (direct && h.len <= direct_cap) rr = io::recv_all(fd_, direct, h.len, timeout_ms);
        else if (out) { out->resize(h.len); rr = io::recv_all(fd_, out->data(), h.len, timeout_ms); }
        else return fail(EPROTO, true);   // 受け皿の無い payload (想定外)
        if (rr <= 0) return fail(rr == 0 ? EPIPE : (errno == ETIMEDOUT ? ETIMEDOUT : EPIPE), true);
      }
      return rep->rc;
    }
    if (h.op == kDevRead || h.op == kDevWrite || h.op == kDevFlush) {
      if (!handle_callback(h, timeout_ms)) { int e = errno; return fail(e == ETIMEDOUT ? ETIMEDOUT : e == EPROTO ? EPROTO : EPIPE, true); }
      continue;
    }
    return fail(EPROTO, true);
  }
}

int RpcVolume::init(int fd, pid_t pid, BlockDevicePtr dev, const MountOptions& opt) {
  fd_ = fd;
  pid_ = pid;
  dev_ = std::move(dev);
  opt_ = opt;
  HelloMsg hm{};
  hm.magic = kMagic;
  hm.version = kVersion;
  hm.max_payload = kMaxPayload;
  std::strncpy(hm.name, "usbmem-app", sizeof hm.name - 1);
  Reply r;
  std::vector<uint8_t> p;
  int64_t rc = call(kHello, 0, 0, 0, &hm, sizeof hm, &r, &p, nullptr, 0, opt.mount_timeout_ms > 0 ? 15000 : 0);
  if (rc < 0) return static_cast<int>(-rc);
  HelloMsg sm{};
  if (p.size() != sizeof sm) return EPROTO;
  std::memcpy(&sm, p.data(), sizeof sm);
  if (sm.magic != kMagic || sm.version != kVersion) return EPROTONOSUPPORT;
  MountReq mr{};
  mr.flags = (opt.rw ? kMountRw : 0u) | (opt.allow_noload ? kMountAllowNoload : 0u) | (opt.allow_dirty ? kMountAllowDirty : 0u);
  mr.block_size = dev_->logical_block_size();
  mr.dev_size = dev_->size();
  mr.tz_minutes_west = opt.tz_minutes_west;
  mr.dev_read_only = dev_->is_read_only() ? 1 : 0;
  p.clear();
  rc = call(kMount, 0, 0, 0, &mr, sizeof mr, &r, &p, nullptr, 0, opt.mount_timeout_ms);
  if (rc < 0) return static_cast<int>(-rc);
  MountRep rep{};
  if (p.size() != sizeof rep) return EPROTO;
  std::memcpy(&rep, p.data(), sizeof rep);
  copy_cstr(&fs_name_, rep.info.fs_name, sizeof rep.info.fs_name);
  copy_cstr(&fs_type_, rep.fs_type, sizeof rep.fs_type);
  root_ = rep.info.root;
  return 0;
}

std::unique_ptr<RpcVolume> RpcVolume::mount(const std::string& exe, BlockDevicePtr dev, const MountOptions& opt, int* err) {
  int fd = -1;
  pid_t pid = -1;
  int rc = spawn(exe, opt.kfsd_args, &fd, &pid);
  if (rc != 0) { if (err) *err = kErrNoKfsd; return nullptr; }
  return mount_fd(fd, pid, std::move(dev), opt, err);
}

std::unique_ptr<RpcVolume> RpcVolume::mount_fd(int fd, pid_t pid, BlockDevicePtr dev, const MountOptions& opt, int* err) {
  std::unique_ptr<RpcVolume> v(new RpcVolume());
  int rc = dev ? v->init(fd, pid, std::move(dev), opt) : EINVAL;
  if (rc != 0) {
    if (!v->dev_) ::close(fd);
    v->reap(false);   // マウントに失敗した kfsd は終わっている (または殺す)
    if (err) *err = rc;
    return nullptr;
  }
  return v;
}

bool RpcVolume::writable() const {
  Reply r;
  std::vector<uint8_t> p;
  if (const_cast<RpcVolume*>(this)->call(kInfo, 0, 0, 0, nullptr, 0, &r, &p) != 0 || p.size() != sizeof(InfoRep)) return false;
  InfoRep i;
  std::memcpy(&i, p.data(), sizeof i);
  return i.writable != 0;
}
uint32_t RpcVolume::state() const {
  Reply r;
  std::vector<uint8_t> p;
  if (const_cast<RpcVolume*>(this)->call(kInfo, 0, 0, 0, nullptr, 0, &r, &p) != 0 || p.size() != sizeof(InfoRep)) return 0;
  InfoRep i;
  std::memcpy(&i, p.data(), sizeof i);
  return i.state;
}

int RpcVolume::simple_name_op(uint32_t op, uint64_t a, uint64_t b, const std::string& name, uint64_t* ino_out) {
  if (!valid_name(name.data(), name.size())) return -EINVAL;
  Reply r;
  int64_t rc = call(op, a, b, 0, name.data(), name.size(), &r);
  if (rc == 0 && ino_out) *ino_out = r.b;
  return static_cast<int>(rc);
}

int RpcVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) { return simple_name_op(kLookup, dir, 0, name, ino); }

int RpcVolume::stat(uint64_t ino, FsStat* st) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!stat_cache_.empty()) {
      auto it = stat_cache_.find(ino);
      if (it != stat_cache_.end()) { *st = it->second; stat_hits_++; return 0; }
    }
  }
  Reply r;
  std::vector<uint8_t> p;
  int64_t rc = call(kStat, ino, 0, 0, nullptr, 0, &r, &p);
  if (rc != 0) return static_cast<int>(rc);
  WireStat w;
  if (p.size() != sizeof w) return static_cast<int>(fail(EPROTO, true));
  std::memcpy(&w, p.data(), sizeof w);
  st->ino = w.ino; st->size = w.size; st->mode = w.mode; st->nlink = w.nlink; st->mtime = w.mtime;
  return 0;
}

int RpcVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  uint64_t cookie = 0;
  const bool with_stat = opt_.prefetch_stat;
  const uint64_t gen0 = gen_.load();
  std::vector<std::pair<uint64_t, FsStat>> stats;
  for (;;) {
    Reply r;
    std::vector<uint8_t> p;
    int64_t rc = call(kReaddir, dir, cookie, with_stat ? static_cast<uint64_t>(kReaddirWithStat) : 0u, nullptr, 0, &r, &p);
    if (rc != 0) { out->clear(); return static_cast<int>(rc); }
    auto bad = [&]() { out->clear(); return static_cast<int>(fail(EPROTO, true)); };   // 信頼しない kfsd の壊れた応答: 接続を捨てる
    size_t o = 0;
    while (o < p.size()) {
      WireDirentHead hd;
      if (p.size() - o < sizeof hd) return bad();
      std::memcpy(&hd, &p[o], sizeof hd);
      o += sizeof hd;
      if (hd.name_len > p.size() - o) return bad();
      const char* np = reinterpret_cast<const char*>(&p[o]);
      // 信頼しない kfsd が返した名前: 空 / '/' / NUL を含むものは受け付けない (パスの組み立てを壊すため)
      if (!valid_name(np, hd.name_len)) return bad();
      FsDirEntry e;
      e.ino = hd.ino;
      e.dtype = hd.dtype & 0xFF;
      e.name.assign(np, hd.name_len);
      o += hd.name_len;
      if (hd.dtype & kDirentHasStat) {
        WireStat w;
        if (p.size() - o < sizeof w) return bad();
        std::memcpy(&w, &p[o], sizeof w);
        o += sizeof w;
        FsStat s;
        s.ino = w.ino; s.size = w.size; s.mode = w.mode; s.nlink = w.nlink; s.mtime = w.mtime;
        stats.emplace_back(e.ino, s);
      }
      out->push_back(std::move(e));
    }
    if (r.c == 0) break;
    if (r.b <= cookie) return bad();   // cookie は必ず進む (進まなければ無限ループ)
    cookie = r.b;
  }
  if (!stats.empty()) {
    std::lock_guard<std::mutex> g(mu_);
    if (gen_.load() == gen0) {   // この readdir の間に変更系が走っていなければ (古い stat を入れない)
      if (stat_cache_.size() > 200000) stat_cache_.clear();
      for (auto& s : stats) stat_cache_[s.first] = s.second;
    }
  }
  return 0;
}

int64_t RpcVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t done = 0;
  while (done < len) {
    size_t n = std::min<size_t>(len - done, kMaxIo);
    Reply r;
    int64_t rc = call(kRead, ino, off + done, n, nullptr, 0, &r, nullptr, p + done, n);
    if (rc < 0) return done ? static_cast<int64_t>(done) : rc;
    if (static_cast<uint64_t>(rc) > n || r.len != static_cast<uint64_t>(rc)) { fail(EPROTO, true); return -EPROTO; }
    done += static_cast<size_t>(rc);
    if (static_cast<size_t>(rc) < n) break;   // EOF (短い読み)
  }
  return static_cast<int64_t>(done);
}

int RpcVolume::readlink(uint64_t ino, std::string* out) {
  Reply r;
  std::vector<uint8_t> p;
  int64_t rc = call(kReadlink, ino, 0, 0, nullptr, 0, &r, &p);
  if (rc == 0) out->assign(p.begin(), p.end());
  return static_cast<int>(rc);
}

int RpcVolume::sync() {
  Reply r;
  return static_cast<int>(call(kSync, 0, 0, 0, nullptr, 0, &r));
}

int RpcVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return simple_name_op(kCreate, dir, mode, name, ino); }
int RpcVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return simple_name_op(kMkdir, dir, mode, name, ino); }
int RpcVolume::unlink(uint64_t dir, const std::string& name) { return simple_name_op(kUnlink, dir, 0, name, nullptr); }
int RpcVolume::rmdir(uint64_t dir, const std::string& name) { return simple_name_op(kRmdir, dir, 0, name, nullptr); }

int RpcVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  if (!valid_name(on.data(), on.size()) || !valid_name(nn.data(), nn.size())) return -EINVAL;
  std::string pl(8, '\0');
  uint32_t l1 = static_cast<uint32_t>(on.size()), l2 = static_cast<uint32_t>(nn.size());
  std::memcpy(&pl[0], &l1, 4);
  std::memcpy(&pl[4], &l2, 4);
  pl += on;
  pl += nn;
  Reply r;
  return static_cast<int>(call(kRename, od, nd, 0, pl.data(), pl.size(), &r));
}

int64_t RpcVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  auto* p = static_cast<const uint8_t*>(buf);
  size_t done = 0;
  do {
    size_t n = std::min<size_t>(len - done, kMaxIo);
    Reply r;
    int64_t rc = call(kWrite, ino, off + done, 0, p + done, n, &r);
    if (rc < 0) return done ? static_cast<int64_t>(done) : rc;
    if (static_cast<uint64_t>(rc) > n) { fail(EPROTO, true); return -EPROTO; }
    done += static_cast<size_t>(rc);
    if (static_cast<size_t>(rc) < n) break;
  } while (done < len);
  return static_cast<int64_t>(done);
}

int64_t RpcVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) {
  auto* p = static_cast<const uint8_t*>(buf);
  size_t done = 0;
  do {
    size_t n = std::min<size_t>(len - done, kMaxIo);
    Reply r;
    int64_t rc = call(kWriteInplace, ino, off + done, 0, p + done, n, &r);
    if (rc < 0) return done ? static_cast<int64_t>(done) : rc;
    if (static_cast<uint64_t>(rc) > n) { fail(EPROTO, true); return -EPROTO; }
    done += static_cast<size_t>(rc);
    if (static_cast<size_t>(rc) < n) break;
  } while (done < len);
  return static_cast<int64_t>(done);
}

int RpcVolume::truncate(uint64_t ino, uint64_t size) {
  Reply r;
  return static_cast<int>(call(kTruncate, ino, size, 0, nullptr, 0, &r));
}

int RpcVolume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  if (!valid_name(name.data(), name.size()) || target.empty() || target.size() > 4096) return -EINVAL;
  std::string pl(4, '\0');
  uint32_t l = static_cast<uint32_t>(name.size());
  std::memcpy(&pl[0], &l, 4);
  pl += name;
  pl += target;
  Reply r;
  int64_t rc = call(kSymlink, dir, 0, 0, pl.data(), pl.size(), &r);
  if (rc == 0 && ino) *ino = r.b;
  return static_cast<int>(rc);
}

int RpcVolume::link(uint64_t dir, const std::string& name, uint64_t target_ino) { return simple_name_op(kLink, dir, target_ino, name, nullptr); }

void RpcVolume::dev_stats(uint64_t* rc, uint64_t* rb, uint64_t* wc, uint64_t* wb) const {
  if (rc) *rc = dev_rc_.load();
  if (rb) *rb = dev_rb_.load();
  if (wc) *wc = dev_wc_.load();
  if (wb) *wb = dev_wb_.load();
}

int RpcVolume::remote_stats(StatsRep* out) {
  Reply r;
  std::vector<uint8_t> p;
  int64_t rc = call(kStats, 0, 0, 0, nullptr, 0, &r, &p);
  if (rc != 0) return static_cast<int>(rc);
  if (p.size() != sizeof *out) return -EPROTO;
  std::memcpy(out, p.data(), sizeof *out);
  return 0;
}

}  // namespace kfsd
}  // namespace usbmem
