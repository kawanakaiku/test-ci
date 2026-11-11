/* SPDX-License-Identifier: GPL-2.0-or-later */
/* exFAT ボリュームをホストから使うための C API (LinuxCompat + Linux fs/exfat の上)。現状は**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: exFAT の inode 番号は実行時に採番される (ディスク上の値ではない)。lookup / readdir が返した ino は、
 * ボリュームを unmount するまで有効 (内部で inode を保持する)。ルートは USBMEM_EXFAT_ROOT_INO 固定。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_EXFAT_C_H
#define USBMEM_EXFAT_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_exfat usbmem_exfat;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_exfat_stat;
typedef int (*usbmem_exfat_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

#define USBMEM_EXFAT_ROOT_INO 1u

/* read_only = 0 は -EROFS (書き込みは未対応)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_exfat_mount(struct usbmem_bdev *bdev, int read_only, usbmem_exfat **out);
void usbmem_exfat_unmount(usbmem_exfat *);
int usbmem_exfat_lookup(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_exfat_stat_ino(usbmem_exfat *, uint64_t ino, usbmem_exfat_stat *st);
int usbmem_exfat_readdir(usbmem_exfat *, uint64_t dir_ino, usbmem_exfat_dir_cb cb, void *user);
int64_t usbmem_exfat_read(usbmem_exfat *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
