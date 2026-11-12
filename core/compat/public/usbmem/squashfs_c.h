/* SPDX-License-Identifier: GPL-2.0-or-later */
/* squashfs ボリュームをホストから使うための C API (LinuxCompat + Linux fs/squashfs の上)。**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: squashfs の i_ino はイメージ内の inode 番号 (ルートも含め値は一定でない)。ルートは usbmem_squashfs_root_ino() で得る。
 * lookup / readdir が返した ino は、ボリュームを unmount するまで有効 (内部で inode を保持する)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_SQUASHFS_C_H
#define USBMEM_SQUASHFS_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_squashfs usbmem_squashfs;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_squashfs_stat;
typedef int (*usbmem_squashfs_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

/* read_only = 0 は -EROFS (squashfs は読み取り専用の形式)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_squashfs_mount(struct usbmem_bdev *bdev, int read_only, usbmem_squashfs **out);
void usbmem_squashfs_unmount(usbmem_squashfs *);
uint64_t usbmem_squashfs_root_ino(usbmem_squashfs *);
int usbmem_squashfs_lookup(usbmem_squashfs *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_squashfs_stat_ino(usbmem_squashfs *, uint64_t ino, usbmem_squashfs_stat *st);
int usbmem_squashfs_readdir(usbmem_squashfs *, uint64_t dir_ino, usbmem_squashfs_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。特殊ファイルは -EINVAL。 */
int64_t usbmem_squashfs_read(usbmem_squashfs *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
