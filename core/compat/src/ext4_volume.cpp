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

std::unique_ptr<Ext4Volume> Ext4Volume::mount(BlockDevicePtr dev, int* err, const Ext4MountOptions& opts) {
  std::unique_ptr<Ext4Volume> vol(new Ext4Volume());
  vol->impl_.reset(new Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ext4_mount(vol->impl_->bd, 1, opts.allow_noload ? 1 : 0, &vol->impl_->v);
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

}  // namespace usbmem
