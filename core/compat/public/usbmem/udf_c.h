/* SPDX-License-Identifier: GPL-2.0-or-later */
/* UDF ボリュームをホストから使うための C API (LinuxCompat + Linux fs/udf)。**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino は udf の inode 番号 (= ICB (File Entry) の物理ブロック番号)。ルートは usbmem_udf_root_ino() で得る。
 * ino から inode を引くとき、物理ブロックを含むパーティションを探して論理アドレスに戻す (docs/isofs-port.md / docs/udf-port.md)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_UDF_C_H
#define USBMEM_UDF_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_udf usbmem_udf;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_udf_stat;
typedef int (*usbmem_udf_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

/* read_only = 0 は -EROFS。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。名前は UTF-8 で返る (UDF の dstring = OSTA CS0 から変換)。 */
int usbmem_udf_mount(struct usbmem_bdev *bdev, int read_only, usbmem_udf **out);
void usbmem_udf_unmount(usbmem_udf *);
uint64_t usbmem_udf_root_ino(usbmem_udf *);
int usbmem_udf_lookup(usbmem_udf *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_udf_stat_ino(usbmem_udf *, uint64_t ino, usbmem_udf_stat *st);
int usbmem_udf_readdir(usbmem_udf *, uint64_t dir_ino, usbmem_udf_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。 */
int64_t usbmem_udf_read(usbmem_udf *, uint64_t ino, uint64_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
