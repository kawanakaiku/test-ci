// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/erofs_volume.h"

#include <cerrno>

#include "usbmem/erofs_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct ErofsVolume::Impl {
  usbmem_erofs* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<ErofsVolume> ErofsVolume::mount(BlockDevicePtr dev, int* err) {
  std::unique_ptr<ErofsVolume> vol(new ErofsVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_erofs_mount(vol->impl_->bd, 1, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

ErofsVolume::~ErofsVolume() {
  if (!impl_) return;
  usbmem_erofs_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

uint64_t ErofsVolume::root() const { return usbmem_erofs_root_ino(impl_->v); }
int ErofsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_erofs_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int ErofsVolume::stat(uint64_t ino, FsStat* st) {
  usbmem_erofs_stat s;
  int rc = usbmem_erofs_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int ErofsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_erofs_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t ErofsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_erofs_read(impl_->v, ino, off, buf, len);
}
int ErofsVolume::readlink(uint64_t ino, std::string* target) {
  FsStat st;
  int rc = stat(ino, &st);
  if (rc) return rc;
  if ((st.mode & 0170000) != 0120000) return -EINVAL;
  std::string buf(static_cast<size_t>(st.size < 4096 ? 4096 : st.size) + 1, '\0');
  int64_t n = usbmem_erofs_read(impl_->v, ino, 0, &buf[0], buf.size());
  if (n < 0) return static_cast<int>(n);
  buf.resize(static_cast<size_t>(n));
  *target = std::move(buf);
  return 0;
}

}  // namespace usbmem
