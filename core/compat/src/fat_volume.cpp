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

std::unique_ptr<FatVolume> FatVolume::mount(BlockDevicePtr dev, int* err) {
  std::unique_ptr<FatVolume> vol(new FatVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_fat_mount(vol->impl_->bd, 1, &vol->impl_->v);
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

}  // namespace usbmem
