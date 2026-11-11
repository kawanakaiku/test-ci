/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ext2 ボリュームをホストから使うための C API (LinuxCompat + Linux fs/ext2 の上)。読み取りは常に可。書き込みはビルドと実行時の両方で有効にしたときだけ (下記)。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino はディスク上の inode 番号 (ルート = 2)。ext2_iget(sb, ino) で直接引ける (範囲外・未使用は -ESTALE / -EFSCORRUPTED)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_EXT2_C_H
#define USBMEM_EXT2_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_ext2 usbmem_ext2;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_ext2_stat;
typedef int (*usbmem_ext2_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

#define USBMEM_EXT2_ROOT_INO 2u

/* read_only = 0 で書き込み可能なマウントを要求する (ビルドで USBMEM_NTFS_WRITE=ON が必要。無効なら -EROFS)。
 * 実際に書き込めるかは usbmem_ext2_writable() で見る: s_state が VALID_FS でない (前回きれいに unmount されていない / エラーあり)、
 * 未知の ro_compat 機能がある、などでは読み取り専用のままマウントに成功する。マウント自体はメディアに何も書かない
 * (最初の書き込みの直前にダーティ印を確定してから書く。docs/ext2-write.md)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。 */
int usbmem_ext2_mount(struct usbmem_bdev *bdev, int read_only, usbmem_ext2 **out);
int usbmem_ext2_writable(usbmem_ext2 *);
void usbmem_ext2_unmount(usbmem_ext2 *);
int usbmem_ext2_lookup(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_ext2_stat_ino(usbmem_ext2 *, uint64_t ino, usbmem_ext2_stat *st);
int usbmem_ext2_readdir(usbmem_ext2 *, uint64_t dir_ino, usbmem_ext2_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。 */
int64_t usbmem_ext2_read(usbmem_ext2 *, uint64_t ino, uint64_t off, void *buf, size_t len);

/* ---- 書き込み (writable() のときだけ。それ以外は -EROFS)。名前は UTF-8 のバイト列 (最大 255 バイト)。 */
int usbmem_ext2_sync(usbmem_ext2 *);   /* dirty なデータ・メタデータ・スーパーブロックをデバイスへ書いて flush */
int usbmem_ext2_create(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_ext2_mkdir(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_ext2_unlink(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_ext2_rmdir(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_ext2_rename(usbmem_ext2 *, uint64_t old_dir, const char *old_name, size_t old_len, uint64_t new_dir, const char *new_name, size_t new_len);
int64_t usbmem_ext2_write(usbmem_ext2 *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 拡張・穴あけ可。戻り値: 書けたバイト数 */
int64_t usbmem_ext2_write_inplace(usbmem_ext2 *, uint64_t ino, uint64_t off, const void *buf, size_t len);   /* 既存サイズの範囲内だけ */
int usbmem_ext2_truncate(usbmem_ext2 *, uint64_t ino, uint64_t size);
/* リンク: シンボリックリンクの読み取りは読み取り専用でも可。作成は書き込み。 */
int usbmem_ext2_readlink(usbmem_ext2 *, uint64_t ino, char *buf, size_t cap);   /* 戻り値: 長さ (NUL なし) / 負 = -errno */
int usbmem_ext2_symlink(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len, const char *target, size_t target_len, uint64_t *ino);
int usbmem_ext2_link(usbmem_ext2 *, uint64_t dir_ino, const char *name, size_t len, uint64_t target_ino);   /* ハードリンク (ディレクトリ不可) */

#ifdef __cplusplus
}
#endif
#endif
