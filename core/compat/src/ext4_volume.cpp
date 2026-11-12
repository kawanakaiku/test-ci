// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ext4_volume.h"

#include <cerrno>

#include "usbmem/ext4_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct Ext4Volume::Impl {
  usbmem_ext4* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<Ext4Volume> Ext4Volume::mount(BlockDevicePtr dev, int* err, const Ext4MountOptions& opts) { return mount_impl(std::move(dev), 1, err, opts); }
std::unique_ptr<Ext4Volume> Ext4Volume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 0, err, {}); }
std::unique_ptr<Ext4Volume> Ext4Volume::mount_impl(BlockDevicePtr dev, int ro, int* err, const Ext4MountOptions& opts) {
  std::unique_ptr<Ext4Volume> vol(new Ext4Volume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ext4_mount(vol->impl_->bd, ro, opts.allow_noload ? 1 : 0, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

Ext4Volume::~Ext4Volume() {
  if (!impl_) return;
  usbmem_ext4_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int Ext4Volume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_ext4_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int Ext4Volume::stat(uint64_t ino, FsStat* st) {
  usbmem_ext4_stat s;
  int rc = usbmem_ext4_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int Ext4Volume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_ext4_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t Ext4Volume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_ext4_read(impl_->v, ino, off, buf, len);
}

int Ext4Volume::readlink(uint64_t ino, std::string* target) {
  char buf[4096];
  int n = usbmem_ext4_readlink(impl_->v, ino, buf, sizeof buf);
  if (n < 0) return n;
  target->assign(buf, static_cast<size_t>(n));
  return 0;
}

bool Ext4Volume::writable() const { return usbmem_ext4_writable(impl_->v) != 0; }
int Ext4Volume::sync() { return usbmem_ext4_sync(impl_->v); }
int Ext4Volume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_ext4_create(impl_->v, dir, name.data(), name.size(), mode, ino); }
int Ext4Volume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_ext4_mkdir(impl_->v, dir, name.data(), name.size(), mode, ino); }
int Ext4Volume::unlink(uint64_t dir, const std::string& name) { return usbmem_ext4_unlink(impl_->v, dir, name.data(), name.size()); }
int Ext4Volume::rmdir(uint64_t dir, const std::string& name) { return usbmem_ext4_rmdir(impl_->v, dir, name.data(), name.size()); }
int Ext4Volume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  return usbmem_ext4_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size());
}
int64_t Ext4Volume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ext4_write(impl_->v, ino, off, buf, len); }
int Ext4Volume::truncate(uint64_t ino, uint64_t size) { return usbmem_ext4_truncate(impl_->v, ino, size); }
int64_t Ext4Volume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ext4_write_inplace(impl_->v, ino, off, buf, len); }
int Ext4Volume::symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) {
  return usbmem_ext4_symlink(impl_->v, dir, name.data(), name.size(), target.data(), target.size(), ino);
}
int Ext4Volume::link(uint64_t dir, const std::string& name, uint64_t target_ino) { return usbmem_ext4_link(impl_->v, dir, name.data(), name.size(), target_ino); }

}  // namespace usbmem
