// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/fat_volume.h"

#include <cerrno>

#include "usbmem/fat_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct FatVolume::Impl {
  usbmem_fat* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<FatVolume> FatVolume::mount(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 1, err); }
std::unique_ptr<FatVolume> FatVolume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 0, err); }
std::unique_ptr<FatVolume> FatVolume::mount_impl(BlockDevicePtr dev, int ro, int* err) {
  std::unique_ptr<FatVolume> vol(new FatVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_fat_mount(vol->impl_->bd, ro, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

FatVolume::~FatVolume() {
  if (!impl_) return;
  usbmem_fat_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int FatVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_fat_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int FatVolume::stat(uint64_t ino, FsStat* st) {
  usbmem_fat_stat s;
  int rc = usbmem_fat_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int FatVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_fat_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t FatVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_fat_read(impl_->v, ino, off, buf, len);
}
bool FatVolume::writable() const { return usbmem_fat_writable(impl_->v) != 0; }
int FatVolume::sync() { return usbmem_fat_sync(impl_->v); }
int FatVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_fat_create(impl_->v, dir, name.data(), name.size(), mode, ino); }
int FatVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_fat_mkdir(impl_->v, dir, name.data(), name.size(), mode, ino); }
int FatVolume::unlink(uint64_t dir, const std::string& name) { return usbmem_fat_unlink(impl_->v, dir, name.data(), name.size()); }
int FatVolume::rmdir(uint64_t dir, const std::string& name) { return usbmem_fat_rmdir(impl_->v, dir, name.data(), name.size()); }
int FatVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  return usbmem_fat_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size());
}
int64_t FatVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_fat_write(impl_->v, ino, off, buf, len); }
int FatVolume::truncate(uint64_t ino, uint64_t size) { return usbmem_fat_truncate(impl_->v, ino, size); }
int64_t FatVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_fat_write_inplace(impl_->v, ino, off, buf, len); }

}  // namespace usbmem
