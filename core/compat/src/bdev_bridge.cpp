// SPDX-License-Identifier: Apache-2.0
// LinuxCompat の block_device ↔ usbmem::BlockDevice ブリッジ。
// 注意: Linux 互換ヘッダ(min/max/swap マクロ)を include しないこと。C++ 標準ヘッダと衝突する。
#include <cerrno>
#include <cstdint>
#include <memory>

#include "usbmem/block_device.h"
#include "usbmem/ntfs_c.h"

struct usbmem_bdev {
  usbmem::BlockDevicePtr dev;
};

extern "C" {
struct usbmem_bdev *usbmem_bdev_wrap(void *p) {
  auto *sp = static_cast<usbmem::BlockDevicePtr *>(p);
  return new (std::nothrow) usbmem_bdev{*sp};
}
void usbmem_bdev_free(struct usbmem_bdev *b) { delete b; }

static int to_errno(const usbmem::Status &s) {
  if (s.ok()) return 0;
  int e = s.sys_errno ? s.sys_errno : (s.code == usbmem::Errc::OutOfRange ? EINVAL : EIO);
  return e > 0 ? -e : e;  // カーネル規約: 負の errno
}
int usbmem_bdev_read(struct usbmem_bdev *b, uint64_t off, void *buf, size_t len) { return to_errno(b->dev->read_at(off, buf, len)); }
int usbmem_bdev_write(struct usbmem_bdev *b, uint64_t off, const void *buf, size_t len) { return to_errno(b->dev->write_at(off, buf, len)); }
int usbmem_bdev_flush(struct usbmem_bdev *b) { return to_errno(b->dev->flush()); }
uint64_t usbmem_bdev_size(struct usbmem_bdev *b) { return b->dev->size(); }
unsigned usbmem_bdev_lbs(struct usbmem_bdev *b) { return b->dev->logical_block_size(); }
}
