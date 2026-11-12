/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISO 9660 (Joliet / Rock Ridge 含む) ボリュームをホストから使うための C API (LinuxCompat + Linux fs/isofs)。**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino は isofs の inode 番号 (= (ブロック << (bufbits-5)) | (レコード位置 >> 5))。ルートは usbmem_isofs_root_ino() で得る。
 * ino から inode を引くとき、ブロックを読んでディレクトリレコードの正確な位置を求める (docs/optical-port.md)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_ISOFS_C_H
#define USBMEM_ISOFS_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_isofs usbmem_isofs;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_isofs_stat;
typedef int (*usbmem_isofs_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

/* read_only = 0 は -EROFS。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。名前は UTF-8 で返る (Joliet は UTF-16BE から変換、Rock Ridge は盤上のバイト列のまま)。 */
int usbmem_isofs_mount(struct usbmem_bdev *bdev, int read_only, usbmem_isofs **out);
void usbmem_isofs_unmount(usbmem_isofs *);
uint64_t usbmem_isofs_root_ino(usbmem_isofs *);
int usbmem_isofs_lookup(usbmem_isofs *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_isofs_stat_ino(usbmem_isofs *, uint64_t ino, usbmem_isofs_stat *st);
int usbmem_isofs_readdir(usbmem_isofs *, uint64_t dir_ino, usbmem_isofs_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト (zisofs 圧縮ファイルは -EOPNOTSUPP)。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。 */
int64_t usbmem_isofs_read(usbmem_isofs *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
