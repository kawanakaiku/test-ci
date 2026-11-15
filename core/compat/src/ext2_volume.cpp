// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ext2_volume.h"

#include <cerrno>

#include "usbmem/ext2_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct Ext2Volume::Impl {
  usbmem_ext2* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<Ext2Volume> Ext2Volume::mount(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 1, err); }
std::unique_ptr<Ext2Volume> Ext2Volume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 0, err); }
std::unique_ptr<Ext2Volume> Ext2Volume::mount_impl(BlockDevicePtr dev, int ro, int* err) {
  if (!ro && dev && dev->is_read_only()) ro = 1;  // 下のデバイス (暗号層など) が読み取り専用なら、mount_rw でも読み取り専用でマウントする (ext4 の writable_precheck と同じ)
  std::unique_ptr<Ext2Volume> vol(new Ext2Volume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ext2_mount(vol->impl_->bd, ro, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

Ext2Volume::~Ext2Volume() {
  if (!impl_) return;
  usbmem_ext2_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int Ext2Volume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_ext2_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int Ext2Volume::stat(uint64_t ino, FsStat* st) {
  usbmem_ext2_stat s;
  int rc = usbmem_ext2_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int Ext2Volume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_ext2_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t Ext2Volume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_ext2_read(impl_->v, ino, off, buf, len);
}
bool Ext2Volume::writable() const { return usbmem_ext2_writable(impl_->v) != 0; }
int Ext2Volume::sync() { return usbmem_ext2_sync(impl_->v); }
int Ext2Volume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_ext2_create(impl_->v, dir, name.data(), name.size(), mode, ino); }
int Ext2Volume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_ext2_mkdir(impl_->v, dir, name.data(), name.size(), mode, ino); }
int Ext2Volume::unlink(uint64_t dir, const std::string& name) { return usbmem_ext2_unlink(impl_->v, dir, name.data(), name.size()); }
int Ext2Volume::rmdir(uint64_t dir, const std::string& name) { return usbmem_ext2_rmdir(impl_->v, dir, name.data(), name.size()); }
int Ext2Volume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  return usbmem_ext2_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size());
}
int64_t Ext2Volume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ext2_write(impl_->v, ino, off, buf, len); }
int Ext2Volume::truncate(uint64_t ino, uint64_t size) { return usbmem_ext2_truncate(impl_->v, ino, size); }
int64_t Ext2Volume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ext2_write_inplace(impl_->v, ino, off, buf, len); }
int Ext2Volume::readlink(uint64_t ino, std::string* target) {
  char buf[4096];
  int n = usbmem_ext2_readlink(impl_->v, ino, buf, sizeof buf);
  if (n < 0) return n;
  target->assign(buf, static_cast<size_t>(n));
  return 0;
}
int Ext2Volume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  return usbmem_ext2_symlink(impl_->v, dir, name.data(), name.size(), target.data(), target.size(), ino);
}
int Ext2Volume::link(uint64_t dir, const std::string& name, uint64_t target_ino) { return usbmem_ext2_link(impl_->v, dir, name.data(), name.size(), target_ino); }

}  // namespace usbmem
