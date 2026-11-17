// SPDX-License-Identifier: GPL-2.0-or-later
#include "fuseshim_volume.h"

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <sys/stat.h>

namespace usbmem {

namespace {
std::string join(const std::string& dir, const std::string& name) { return dir == "/" ? "/" + name : dir + "/" + name; }
unsigned dtype_of(uint32_t mode) {
  switch (mode & S_IFMT) {
    case S_IFDIR: return 4;   // DT_DIR
    case S_IFLNK: return 10;  // DT_LNK
    case S_IFREG: return 8;   // DT_REG
    case S_IFCHR: return 2;
    case S_IFBLK: return 6;
    case S_IFIFO: return 1;
    case S_IFSOCK: return 12;
    default: return 0;
  }
}
FsStat to_stat(const fuseshim_attr& a) {
  FsStat s;
  s.ino = a.ino;
  s.size = a.size;
  s.mode = a.mode;
  s.nlink = a.nlink;
  s.mtime = a.mtime_sec;
  return s;
}
}  // namespace

FuseShimVolume::FuseShimVolume(std::string fs_name) : name_(std::move(fs_name)) {
  fuseshim_attr a;
  if (fuseshim_getattr("/", &a) == 0) {
    root_ = a.ino ? a.ino : 1;
    paths_[root_] = "/";
  }
}

bool FuseShimVolume::path_of(uint64_t ino, std::string* p) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = paths_.find(ino);
  if (it == paths_.end()) return false;
  *p = it->second;
  return true;
}

void FuseShimVolume::remember(uint64_t ino, const std::string& path) {
  std::lock_guard<std::mutex> l(mu_);
  paths_.emplace(ino, path);
}

int FuseShimVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  std::string dp;
  if (!path_of(dir, &dp)) return -ENOENT;
  std::string p = join(dp, name);
  fuseshim_attr a;
  int rc = fuseshim_getattr(p.c_str(), &a);
  if (rc) return rc;
  *ino = a.ino;
  remember(a.ino, p);
  return 0;
}

int FuseShimVolume::stat(uint64_t ino, FsStat* st) {
  std::string p;
  if (!path_of(ino, &p)) return -ENOENT;
  fuseshim_attr a;
  int rc = fuseshim_getattr(p.c_str(), &a);
  if (rc) return rc;
  *st = to_stat(a);
  return 0;
}

int FuseShimVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  std::string dp;
  if (!path_of(dir, &dp)) return -ENOENT;
  struct Ctx {
    std::vector<FsDirEntry>* out;
  } c{out};
  out->clear();
  int rc = fuseshim_readdir(
      dp.c_str(),
      [](void* arg, const char* name, uint64_t ino, uint32_t mode) -> int {
        static_cast<Ctx*>(arg)->out->push_back(FsDirEntry{name, ino, dtype_of(mode)});
        return 0;
      },
      &c);
  if (rc) return rc;
  std::lock_guard<std::mutex> l(mu_);
  for (const auto& e : *out)
    if (e.name != "." && e.name != "..") paths_.emplace(e.ino, join(dp, e.name));
  return 0;
}

int64_t FuseShimVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  std::string p;
  if (!path_of(ino, &p)) return -ENOENT;
  return fuseshim_read(p.c_str(), off, buf, len);
}

int FuseShimVolume::readlink(uint64_t ino, std::string* out) {
  std::string p;
  if (!path_of(ino, &p)) return -ENOENT;
  char buf[4096];
  int rc = fuseshim_readlink(p.c_str(), buf, sizeof buf);
  if (rc) return rc;
  *out = buf;
  return 0;
}

int FuseShimVolume::statfs(fuseshim_statvfs* out) { return fuseshim_statfs("/", out); }

// ---- セッション -------------------------------------------------------------------------------------------------
struct FuseShimSession::State {
  std::mutex m;
  std::condition_variable cv;
  bool ready = false, stop_req = false, done = false;
  int rc = -1;
};

std::unique_ptr<FuseShimSession> FuseShimSession::start(std::function<int()> main_fn, const std::string& fs_name, int* exit_code) {
  std::unique_ptr<FuseShimSession> s(new FuseShimSession);
  s->st_ = std::make_shared<State>();
  auto st = s->st_;
  // fuse_loop の中身: 準備完了を知らせて、stop() まで待つ (その間、別スレッドが fuseshim_* を呼ぶ)
  fuseshim_set_loop_hook(
      [](void* arg) {
        State* t = static_cast<State*>(arg);
        std::unique_lock<std::mutex> l(t->m);
        t->ready = true;
        t->cv.notify_all();
        t->cv.wait(l, [t] { return t->stop_req; });
      },
      st.get());
  s->th_ = std::thread([st, main_fn] {
    int rc = main_fn();
    std::lock_guard<std::mutex> l(st->m);
    st->rc = rc;
    st->done = true;
    st->cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> l(st->m);
    st->cv.wait(l, [&] { return st->ready || st->done; });
    if (!st->ready) {  // fuse_loop に入る前に main が戻った
      int rc = st->rc;
      l.unlock();
      s->th_.join();
      s->stopped_ = true;
      if (exit_code) *exit_code = rc;
      return nullptr;
    }
  }
  s->vol_.reset(new FuseShimVolume(fs_name));
  if (!s->vol_->ok()) {
    s->stop();
    if (exit_code) *exit_code = -1;
    return nullptr;
  }
  return s;
}

int FuseShimSession::stop() {
  if (stopped_) return rc_;
  stopped_ = true;
  {
    std::lock_guard<std::mutex> l(st_->m);
    st_->stop_req = true;
    st_->cv.notify_all();
  }
  if (th_.joinable()) th_.join();
  rc_ = st_->rc;
  return rc_;
}

FuseShimSession::~FuseShimSession() { stop(); }

}  // namespace usbmem
