// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/squashfs_volume.h"

#include <cerrno>

#include "usbmem/squashfs_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct SquashfsVolume::Impl {
  usbmem_squashfs* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<SquashfsVolume> SquashfsVolume::mount(BlockDevicePtr dev, int* err) {
  std::unique_ptr<SquashfsVolume> vol(new SquashfsVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_squashfs_mount(vol->impl_->bd, 1, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

SquashfsVolume::~SquashfsVolume() {
  if (!impl_) return;
  usbmem_squashfs_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

uint64_t SquashfsVolume::root() const { return usbmem_squashfs_root_ino(impl_->v); }
int SquashfsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_squashfs_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int SquashfsVolume::stat(uint64_t ino, FsStat* st) {
  usbmem_squashfs_stat s;
  int rc = usbmem_squashfs_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int SquashfsVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_squashfs_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t SquashfsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_squashfs_read(impl_->v, ino, off, buf, len);
}
int SquashfsVolume::readlink(uint64_t ino, std::string* target) {
  FsStat st;
  int rc = stat(ino, &st);
  if (rc) return rc;
  if ((st.mode & 0170000) != 0120000) return -EINVAL;
  std::string buf(static_cast<size_t>(st.size < 4096 ? 4096 : st.size) + 1, '\0');
  int64_t n = usbmem_squashfs_read(impl_->v, ino, 0, &buf[0], buf.size());
  if (n < 0) return static_cast<int>(n);
  buf.resize(static_cast<size_t>(n));
  *target = std::move(buf);
  return 0;
}

}  // namespace usbmem
