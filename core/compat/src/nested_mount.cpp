// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/nested_mount.h"

#include <algorithm>
#include <cerrno>

namespace usbmem {

// 同じスレッドの再入 (= 子の読み出しが、巡り巡って同じボリュームを読む) は、ロックを取らずに失敗させる (デッドロックさせない)
struct LockedGuard {
  LockedFsVolume* v;
  bool ok = false;
  explicit LockedGuard(LockedFsVolume* p) : v(p) {
    if (v->owner_.load() == std::this_thread::get_id()) return;
    v->mu_.lock();
    v->owner_.store(std::this_thread::get_id());
    ok = true;
  }
  ~LockedGuard() {
    if (ok) { v->owner_.store(std::thread::id()); v->mu_.unlock(); }
  }
  LockedGuard(const LockedGuard&) = delete;
  LockedGuard& operator=(const LockedGuard&) = delete;
};

#define LOCKED(call)                \
  LockedGuard g(this);              \
  if (!g.ok) return -ELOOP;         \
  return inner_->call

int LockedFsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) { LOCKED(lookup(dir, name, ino)); }
int LockedFsVolume::stat(uint64_t ino, FsStat* st) { LOCKED(stat(ino, st)); }
int LockedFsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) { LOCKED(readdir(dir, out)); }
int64_t LockedFsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) { LOCKED(read(ino, off, buf, len)); }
int LockedFsVolume::sync() { LOCKED(sync()); }
int LockedFsVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { LOCKED(create(dir, name, mode, ino)); }
int LockedFsVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { LOCKED(mkdir(dir, name, mode, ino)); }
int LockedFsVolume::unlink(uint64_t dir, const std::string& name) { LOCKED(unlink(dir, name)); }
int LockedFsVolume::rmdir(uint64_t dir, const std::string& name) { LOCKED(rmdir(dir, name)); }
int LockedFsVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) { LOCKED(rename(od, on, nd, nn)); }
int64_t LockedFsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { LOCKED(write(ino, off, buf, len)); }
int LockedFsVolume::truncate(uint64_t ino, uint64_t size) { LOCKED(truncate(ino, size)); }
int64_t LockedFsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { LOCKED(write_inplace(ino, off, buf, len)); }
int LockedFsVolume::readlink(uint64_t ino, std::string* target) { LOCKED(readlink(ino, target)); }
int LockedFsVolume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) { LOCKED(symlink(dir, name, target, ino)); }
int LockedFsVolume::link(uint64_t dir, const std::string& name, uint64_t ino) { LOCKED(link(dir, name, ino)); }

int VolumeFileDevice::open(std::shared_ptr<FsVolume> parent, uint64_t ino, uint32_t block_size, std::shared_ptr<VolumeFileDevice>* out) {
  if (!parent || !out) return EINVAL;
  FsStat st;
  int rc = parent->stat(ino, &st);
  if (rc) return rc < 0 ? -rc : rc;
  if (st.is_dir()) return EISDIR;
  if ((st.mode & 0170000) == 0120000) return EINVAL;  // シンボリックリンク (呼び出し側で辿ってから渡す)
  if (st.size < 512) return EINVAL;
  std::shared_ptr<VolumeFileDevice> d(new VolumeFileDevice());
  d->parent_ = std::move(parent);
  d->ino_ = ino;
  d->size_ = st.size;
  d->mtime_ = st.mtime;
  d->block_size_ = block_size ? block_size : 512;
  d->check_stale_ = d->parent_->writable();
  *out = std::move(d);
  return 0;
}

Status VolumeFileDevice::read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt) {
  if (closed_.load()) return Status::Make(Errc::Closed);
  if (!range_ok(size_, offset, len)) return Status::Make(Errc::OutOfRange);
  if (check_stale_) {
    FsStat st;
    int rc = parent_->stat(ino_, &st);
    if (rc) return Status::Make(Errc::Io, rc < 0 ? -rc : rc);
    if (st.size != size_ || st.mtime != mtime_) return Status::Make(Errc::Io, ESTALE);
  }
  auto* p = static_cast<uint8_t*>(buf);
  constexpr size_t kChunk = 1 << 20;
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    size_t n = std::min(len, kChunk);
    int64_t r = parent_->read(ino_, offset, p, n);
    if (r < 0) return Status::Make(Errc::Io, static_cast<int>(-r));
    if (r == 0) return Status::Make(Errc::Io, EIO);  // 開いたあとで短くなった (親が書き込み可能で、チェックをすり抜けた)
    p += r;
    offset += static_cast<uint64_t>(r);
    len -= static_cast<size_t>(r);
  }
  return Status::Ok();
}

std::string VolumeFileDevice::describe() const {
  return std::string("nested:") + parent_->fs_name() + "#" + std::to_string(ino_);
}

NestedOpen open_nested_image(const std::shared_ptr<FsVolume>& parent, const std::shared_ptr<const NestedLineage>& parent_lineage,
                             const std::string& path, int max_depth) {
  NestedOpen r;
  auto fail = [&](int e, const char* why) { r.err = e; r.why = why; return r; };
  if (!parent) return fail(EINVAL, "no parent volume");
  int depth = parent_lineage ? parent_lineage->depth : 0;
  if (depth + 1 > max_depth) return fail(ELOOP, "nesting too deep");
  uint64_t ino = 0;
  int rc = parent->resolve(path, &ino);
  if (rc) return fail(rc < 0 ? -rc : rc, "path not found in parent volume");
  if (parent_lineage)
    for (const auto& l : parent_lineage->chain)
      if (l.volume == parent.get() && l.ino == ino) return fail(ELOOP, "cycle: the file already backs an ancestor volume");
  std::shared_ptr<VolumeFileDevice> d;
  rc = VolumeFileDevice::open(parent, ino, 512, &d);
  if (rc) return fail(rc, rc == EISDIR ? "is a directory" : rc == EINVAL ? "not a regular file or too small" : "cannot open the file");
  auto lin = std::make_shared<NestedLineage>();
  lin->depth = depth + 1;
  if (parent_lineage) lin->chain = parent_lineage->chain;
  lin->chain.push_back({parent.get(), ino});
  r.dev = std::move(d);
  r.lineage = std::move(lin);
  return r;
}

}  // namespace usbmem
