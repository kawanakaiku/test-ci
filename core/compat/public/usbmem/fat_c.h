/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FAT ボリュームをホストから使うための C API (LinuxCompat + Linux fs/fat の上)。読み取りは常に可。書き込みはビルドと実行時の両方で有効にしたときだけ (下記。docs/fat-write.md)。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino について: FAT の inode 番号は実行時に採番される (ディスク上の値ではない)。lookup / readdir が返した ino は、
 * ボリュームを unmount するまで (またはそのファイル・ディレクトリを消すまで) 有効 (内部で inode を保持する)。ルートは USBMEM_FAT_ROOT_INO 固定。
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

/* read_only = 0 で書き込み可能なマウントを要求する (ビルドで USBMEM_NTFS_WRITE=ON が必要。無効なら -EROFS)。
 * 実際に書き込めるかは usbmem_fat_writable() で見る: ブートセクタのダーティビット (前回きれいに取り外されていない) / メディアエラー /
 * FAT[1] のクリーン bit (FAT16/32) / FAT が 3 面以上などでは、読み取り専用のままマウントに成功する。マウント自体はメディアに何も書かない
 * (最初の書き込みの直前にダーティ印を確定してから書く)。
 * usbmem_fat_set_timezone: FAT の時刻 (端末のローカル時刻で記録される) を UTC に直すための、UTC より何分西か (日本は -540)。マウント前に設定する。 */
void usbmem_fat_set_timezone(int minutes_west);
/* bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_fat_mount(struct usbmem_bdev *bdev, int read_only, usbmem_fat **out);
int usbmem_fat_writable(usbmem_fat *);
void usbmem_fat_unmount(usbmem_fat *);
int usbmem_fat_lookup(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_fat_stat_ino(usbmem_fat *, uint64_t ino, usbmem_fat_stat *st);
int usbmem_fat_readdir(usbmem_fat *, uint64_t dir_ino, usbmem_fat_dir_cb cb, void *user);
int64_t usbmem_fat_read(usbmem_fat *, uint64_t ino, uint64_t off, void *buf, size_t len);

/* ---- 書き込み (writable() のときだけ。それ以外は -EROFS)。名前は UTF-8 のバイト列 (長い名前は UTF-16 で保存。絵文字はサロゲートペア)。FAT の名前は大文字小文字を区別しない (lookup / 重複判定も)。
 * 4GiB-1 を超えるファイルは -EFBIG。FAT12/16 のルートディレクトリが満杯なら -ENOSPC。 */
int usbmem_fat_sync(usbmem_fat *);   /* dirty なデータ・メタデータをデバイスへ書いて flush (順序は docs/fat-write.md) */
int usbmem_fat_create(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_fat_mkdir(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_fat_unlink(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_fat_rmdir(usbmem_fat *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_fat_rename(usbmem_fat *, uint64_t old_dir, const char *old_name, size_t old_len, uint64_t new_dir, const char *new_name, size_t new_len);
int64_t usbmem_fat_write(usbmem_fat *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 拡張・穴あけ可 (穴はゼロで埋める。FAT はスパースにできない)。戻り値: 書けたバイト数 */
int64_t usbmem_fat_write_inplace(usbmem_fat *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 既存サイズの範囲内だけ */
int usbmem_fat_truncate(usbmem_fat *, uint64_t ino, uint64_t size);

#ifdef __cplusplus
}
#endif
#endif
