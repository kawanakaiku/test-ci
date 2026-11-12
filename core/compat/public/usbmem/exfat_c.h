/* SPDX-License-Identifier: GPL-2.0-or-later */
/* exFAT ボリュームをホストから使うための C API (LinuxCompat + Linux fs/exfat の上)。読み取りは常に可。書き込みはビルドと実行時の両方で有効にしたときだけ (下記。docs/exfat-write.md)。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: exFAT の inode 番号は実行時に採番される (ディスク上の値ではない)。lookup / readdir / create が返した ino は、
 * ボリュームを unmount するまで (またはそのファイル・ディレクトリを消すまで) 有効 (内部で inode を保持する)。ルートは USBMEM_EXFAT_ROOT_INO 固定。
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

/* read_only = 0 で書き込み可能なマウントを要求する (ビルドで USBMEM_NTFS_WRITE=ON が必要。無効なら -EROFS)。
 * 実際に書き込めるかは usbmem_exfat_writable() で見る: ボリュームが「汚れている」(VolumeDirty = 前回きれいに取り外されていない) / MediaFailure /
 * FAT が 2 面 (TexFAT) などでは、読み取り専用のままマウントに成功する。マウント自体はメディアに何も書かない
 * (最初の書き込みの直前に VolumeDirty を確定してから書く)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_exfat_mount(struct usbmem_bdev *bdev, int read_only, usbmem_exfat **out);
int usbmem_exfat_writable(usbmem_exfat *);
void usbmem_exfat_unmount(usbmem_exfat *);
int usbmem_exfat_lookup(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_exfat_stat_ino(usbmem_exfat *, uint64_t ino, usbmem_exfat_stat *st);
int usbmem_exfat_readdir(usbmem_exfat *, uint64_t dir_ino, usbmem_exfat_dir_cb cb, void *user);
int64_t usbmem_exfat_read(usbmem_exfat *, uint64_t ino, uint64_t off, void *buf, size_t len);

/* ---- 書き込み (writable() のときだけ。それ以外は -EROFS)。名前は UTF-8 のバイト列。exFAT の名前は大文字小文字を区別しない (lookup / 重複判定も)。 */
int usbmem_exfat_sync(usbmem_exfat *);   /* dirty なデータ・メタデータをデバイスへ書いて flush (順序は docs/exfat-write.md) */
int usbmem_exfat_create(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_exfat_mkdir(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_exfat_unlink(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_exfat_rmdir(usbmem_exfat *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_exfat_rename(usbmem_exfat *, uint64_t old_dir, const char *old_name, size_t old_len, uint64_t new_dir, const char *new_name, size_t new_len);
int64_t usbmem_exfat_write(usbmem_exfat *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 拡張・穴あけ可。戻り値: 書けたバイト数 */
int64_t usbmem_exfat_write_inplace(usbmem_exfat *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 既存サイズの範囲内だけ */
int usbmem_exfat_truncate(usbmem_exfat *, uint64_t ino, uint64_t size);

#ifdef __cplusplus
}
#endif
#endif
