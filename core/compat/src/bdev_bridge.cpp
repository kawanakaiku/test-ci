// SPDX-License-Identifier: Apache-2.0
// LinuxCompat の block_device ↔ usbmem::BlockDevice ブリッジ。
// 注意: Linux 互換ヘッダ(min/max/swap マクロ)を include しないこと。C++ 標準ヘッダと衝突する。
#include <cstdio>
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

#ifdef __ANDROID__
#include <android/log.h>
#endif
/* 失敗した I/O を 1 行ログに残す (実機で mount が errno だけで失敗したとき、どの読み書きかを特定するため)。連続しても 20 件まで */
static void log_fail(const char *op, uint64_t off, size_t len, const usbmem::Status &s) {
  static int n;
  if (n++ >= 20) return;
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_WARN, "usbmem", "bdev %s failed: off=%llu len=%zu code=%d errno=%d", op, (unsigned long long)off, len, (int)s.code, s.sys_errno);
#else
  fprintf(stderr, "usbmem: bdev %s failed: off=%llu len=%zu code=%d errno=%d\n", op, (unsigned long long)off, len, (int)s.code, s.sys_errno);
#endif
}
static int to_errno(const usbmem::Status &s) {
  if (s.ok()) return 0;
  int e = s.sys_errno ? s.sys_errno : (s.code == usbmem::Errc::OutOfRange ? EINVAL : EIO);
  return e > 0 ? -e : e;  // カーネル規約: 負の errno
}
int usbmem_bdev_read(struct usbmem_bdev *b, uint64_t off, void *buf, size_t len) { auto st = b->dev->read_at(off, buf, len); if (!st.ok()) log_fail("read", off, len, st); return to_errno(st); }
int usbmem_bdev_write(struct usbmem_bdev *b, uint64_t off, const void *buf, size_t len) { auto st = b->dev->write_at(off, buf, len); if (!st.ok()) log_fail("write", off, len, st); return to_errno(st); }
int usbmem_bdev_flush(struct usbmem_bdev *b) { return to_errno(b->dev->flush()); }
uint64_t usbmem_bdev_size(struct usbmem_bdev *b) { return b->dev->size(); }
int usbmem_bdev_is_ro(struct usbmem_bdev *b) { return b->dev->is_read_only() ? 1 : 0; }
unsigned usbmem_bdev_lbs(struct usbmem_bdev *b) { return b->dev->logical_block_size(); }
}
