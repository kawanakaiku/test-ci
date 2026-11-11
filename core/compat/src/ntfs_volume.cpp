// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ntfs_volume.h"

#include <cerrno>

#include "usbmem/ntfs_c.h"

namespace usbmem {

struct NtfsVolume::Impl {
  usbmem_ntfs* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<NtfsVolume> NtfsVolume::mount(BlockDevicePtr dev, int* err) {
  std::unique_ptr<NtfsVolume> vol(new NtfsVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ntfs_mount(vol->impl_->bd, /*read_only=*/1, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

NtfsVolume::~NtfsVolume() {
  if (!impl_) return;
  usbmem_ntfs_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int NtfsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_ntfs_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int NtfsVolume::stat(uint64_t ino, NtfsStat* st) {
  usbmem_ntfs_stat s;
  int rc = usbmem_ntfs_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int NtfsVolume::readdir(uint64_t dir, std::vector<NtfsDirEntry>* out) {
  out->clear();
  return usbmem_ntfs_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<NtfsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t NtfsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_ntfs_read(impl_->v, ino, off, buf, len);
}
int NtfsVolume::resolve(const std::string& path, uint64_t* ino) {
  uint64_t cur = root();
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) {
      int rc = lookup(cur, path.substr(i, j - i), &cur);
      if (rc) return rc;
    }
    i = j;
  }
  *ino = cur;
  return 0;
}

}  // namespace usbmem
