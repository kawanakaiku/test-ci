/* SPDX-License-Identifier: GPL-2.0-or-later */
/* erofs ボリュームをホストから使うための C API (LinuxCompat + Linux fs/erofs の上)。**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: erofs の i_ino はイメージ内の inode 番号 (ルートも含め値は一定でない)。ルートは usbmem_erofs_root_ino() で得る。
 * lookup / readdir が返した ino は、ボリュームを unmount するまで有効 (内部で inode を保持する)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_EROFS_C_H
#define USBMEM_EROFS_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_erofs usbmem_erofs;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_erofs_stat;
typedef int (*usbmem_erofs_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

/* read_only = 0 は -EROFS (erofs は読み取り専用の形式)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_erofs_mount(struct usbmem_bdev *bdev, int read_only, usbmem_erofs **out);
void usbmem_erofs_unmount(usbmem_erofs *);
uint64_t usbmem_erofs_root_ino(usbmem_erofs *);
int usbmem_erofs_lookup(usbmem_erofs *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_erofs_stat_ino(usbmem_erofs *, uint64_t ino, usbmem_erofs_stat *st);
int usbmem_erofs_readdir(usbmem_erofs *, uint64_t dir_ino, usbmem_erofs_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。特殊ファイルは -EINVAL。 */
int64_t usbmem_erofs_read(usbmem_erofs *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
