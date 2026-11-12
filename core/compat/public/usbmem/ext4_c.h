/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ext4 ボリュームをホストから使うための C API (LinuxCompat + Linux fs/ext4 の上)。現状は**読み取り専用**。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない。
 * ino はディスク上の inode 番号 (ルート = 2)。ext4_iget(sb, ino) で直接引ける (範囲外・未使用は -ESTALE / -EFSCORRUPTED)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_EXT4_C_H
#define USBMEM_EXT4_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_ext4 usbmem_ext4;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_ext4_stat;
typedef int (*usbmem_ext4_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

#define USBMEM_EXT4_ROOT_INO 2u

/* read_only = 0 は -EROFS (書き込みは未対応)。bdev は usbmem_bdev_wrap() (ntfs_c.h) で作る。
 * journal は常に再生しない (noload)。マウント前に超ブロックを自前で検査する:
 *   needs_recovery が立っていて allow_noload=0 → -EUCLEAN。未対応機能 (encrypt/casefold/verity/DAX/不明な incompat) → -EOPNOTSUPP。 */
int usbmem_ext4_mount(struct usbmem_bdev *bdev, int read_only, int allow_noload, usbmem_ext4 **out);
void usbmem_ext4_unmount(usbmem_ext4 *);
int usbmem_ext4_lookup(usbmem_ext4 *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_ext4_stat_ino(usbmem_ext4 *, uint64_t ino, usbmem_ext4_stat *st);
int usbmem_ext4_readdir(usbmem_ext4 *, uint64_t dir_ino, usbmem_ext4_dir_cb cb, void *user);
/* 通常ファイル: off から len バイト。シンボリックリンク: リンク先の文字列 (off/len で切り出し)。 */
int64_t usbmem_ext4_read(usbmem_ext4 *, uint64_t ino, uint64_t off, void *buf, size_t len);
/* シンボリックリンクのリンク先 (NUL なし、最大 cap バイト)。戻り値: 長さ / 負 = -errno (リンクでなければ -EINVAL)。 */
int usbmem_ext4_readlink(usbmem_ext4 *, uint64_t ino, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
