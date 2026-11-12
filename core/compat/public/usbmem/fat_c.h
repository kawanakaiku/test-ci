/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT ボリュームをホストから使うための C API (LinuxCompat + Linux fs/fat の上)。現状は**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: FAT の inode 番号は実行時に採番される (ディスク上の値ではない)。lookup / readdir が返した ino は、
 * ボリュームを unmount するまで有効 (内部で inode を保持する)。ルートは USBMEM_FAT_ROOT_INO 固定。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_FAT_C_H
#define USBMEM_FAT_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_fat usbmem_fat;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_fat_stat;
typedef int (*usbmem_fat_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

#define USBMEM_FAT_ROOT_INO 1u

/* read_only = 0 は -EROFS (書き込みは未対応)。usbmem_fat_set_timezone: FAT の時刻 (端末のローカル時刻で記録される) を UTC に直すための、UTC より何分西か (日本は -540)。マウント前に設定する。 */
void usbmem_fat_set_timezone(int minutes_west);
/* bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_fat_mount(struct usbmem_bdev *bdev, int read_only, usbmem_fat **out);
void usbmem_fat_unmount(usbmem_fat *);
int usbmem_fat_lookup(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_fat_stat_ino(usbmem_fat *, uint64_t ino, usbmem_fat_stat *st);
int usbmem_fat_readdir(usbmem_fat *, uint64_t dir_ino, usbmem_fat_dir_cb cb, void *user);
int64_t usbmem_fat_read(usbmem_fat *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
