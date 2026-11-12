// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/udf_volume.h"

#include <cerrno>

#include "usbmem/udf_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct UdfVolume::Impl {
  usbmem_udf* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<UdfVolume> UdfVolume::mount(BlockDevicePtr dev, int* err) {
  std::unique_ptr<UdfVolume> vol(new UdfVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_udf_mount(vol->impl_->bd, 1, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

UdfVolume::~UdfVolume() {
  if (!impl_) return;
  usbmem_udf_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

uint64_t UdfVolume::root() const { return usbmem_udf_root_ino(impl_->v); }
int UdfVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_udf_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int UdfVolume::stat(uint64_t ino, FsStat* st) {
  usbmem_udf_stat s;
  int rc = usbmem_udf_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int UdfVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_udf_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t UdfVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_udf_read(impl_->v, ino, off, buf, len);
}
int UdfVolume::readlink(uint64_t ino, std::string* target) {
  usbmem_udf_stat s;
  int rc = usbmem_udf_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  if ((s.mode & 0170000) != 0120000) return -EINVAL;   // シンボリックリンクではない
  char buf[4096];
  int64_t n = usbmem_udf_read(impl_->v, ino, 0, buf, sizeof buf);
  if (n < 0) return static_cast<int>(n);
  target->assign(buf, static_cast<size_t>(n));
  return 0;
}

}  // namespace usbmem
