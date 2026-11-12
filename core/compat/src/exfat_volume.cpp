// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/exfat_volume.h"

#include <cerrno>

#include "usbmem/exfat_c.h"
#include "usbmem/ntfs_c.h"   // usbmem_bdev_wrap / usbmem_bdev_free

namespace usbmem {

struct ExfatVolume::Impl {
  usbmem_exfat* v = nullptr;
  usbmem_bdev* bd = nullptr;
};

std::unique_ptr<ExfatVolume> ExfatVolume::mount(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 1, err); }
std::unique_ptr<ExfatVolume> ExfatVolume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl(std::move(dev), 0, err); }
std::unique_ptr<ExfatVolume> ExfatVolume::mount_impl(BlockDevicePtr dev, int ro, int* err) {
  std::unique_ptr<ExfatVolume> vol(new ExfatVolume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_exfat_mount(vol->impl_->bd, ro, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  return vol;
}

ExfatVolume::~ExfatVolume() {
  if (!impl_) return;
  usbmem_exfat_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int ExfatVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_exfat_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int ExfatVolume::stat(uint64_t ino, FsStat* st) {
  usbmem_exfat_stat s;
  int rc = usbmem_exfat_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int ExfatVolume::readdir(uint64_t dir, std::vector<FsDirEntry>* out) {
  out->clear();
  return usbmem_exfat_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<FsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t ExfatVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_exfat_read(impl_->v, ino, off, buf, len);
}
bool ExfatVolume::writable() const { return usbmem_exfat_writable(impl_->v) != 0; }
int ExfatVolume::sync() { return usbmem_exfat_sync(impl_->v); }
int ExfatVolume::create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_exfat_create(impl_->v, dir, name.data(), name.size(), mode, ino); }
int ExfatVolume::mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) { return usbmem_exfat_mkdir(impl_->v, dir, name.data(), name.size(), mode, ino); }
int ExfatVolume::unlink(uint64_t dir, const std::string& name) { return usbmem_exfat_unlink(impl_->v, dir, name.data(), name.size()); }
int ExfatVolume::rmdir(uint64_t dir, const std::string& name) { return usbmem_exfat_rmdir(impl_->v, dir, name.data(), name.size()); }
int ExfatVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) {
  return usbmem_exfat_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size());
}
int64_t ExfatVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_exfat_write(impl_->v, ino, off, buf, len); }
int ExfatVolume::truncate(uint64_t ino, uint64_t size) { return usbmem_exfat_truncate(impl_->v, ino, size); }
int64_t ExfatVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_exfat_write_inplace(impl_->v, ino, off, buf, len); }

}  // namespace usbmem
