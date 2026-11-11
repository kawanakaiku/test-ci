// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ntfs_volume.h"

#include <cerrno>

#include "usbmem/ntfs_c.h"

namespace usbmem {

struct NtfsVolume::Impl {
  usbmem_ntfs* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err);
std::unique_ptr<NtfsVolume> NtfsVolume::mount(BlockDevicePtr dev, int* err) { return mount_impl_(std::move(dev), 1, err); }
std::unique_ptr<NtfsVolume> NtfsVolume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl_(std::move(dev), 0, err); }
int64_t NtfsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ntfs_write(impl_->v, ino, off, buf, len); }
int NtfsVolume::create(uint64_t d, const std::string& n, uint32_t m, uint64_t* i) { return usbmem_ntfs_create(impl_->v, d, n.data(), n.size(), m, i); }
int NtfsVolume::mkdir(uint64_t d, const std::string& n, uint32_t m, uint64_t* i) { return usbmem_ntfs_mkdir(impl_->v, d, n.data(), n.size(), m, i); }
int NtfsVolume::unlink(uint64_t d, const std::string& n) { return usbmem_ntfs_unlink(impl_->v, d, n.data(), n.size()); }
int NtfsVolume::rmdir(uint64_t d, const std::string& n) { return usbmem_ntfs_rmdir(impl_->v, d, n.data(), n.size()); }
int NtfsVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) { return usbmem_ntfs_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size()); }
bool NtfsVolume::writable() const { return usbmem_ntfs_writable(impl_->v) != 0; }
int NtfsVolume::truncate(uint64_t ino, uint64_t size) { return usbmem_ntfs_truncate(impl_->v, ino, size); }
int NtfsVolume::sync() { return usbmem_ntfs_sync(impl_->v); }
int64_t NtfsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ntfs_write_inplace(impl_->v, ino, off, buf, len); }
std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err) {
  std::unique_ptr<NtfsVolume> vol(new NtfsVolume());
  vol->impl_.reset(new NtfsVolume::Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ntfs_mount(vol->impl_->bd, ro, &vol->impl_->v);
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
}  // namespace usbmem
