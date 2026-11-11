/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NTFS ボリュームをホストから使うための C API (LinuxCompat + fs/ntfs の上)。
 * 戻り値: 0/正 = 成功、負 = -errno。スレッドセーフではない (Phase 2 は単一スレッド利用)。
 * このヘッダは Linux 互換ヘッダを含まないので C++ から安全に include できる。 */
#ifndef USBMEM_NTFS_C_H
#define USBMEM_NTFS_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct usbmem_ntfs usbmem_ntfs;
struct usbmem_bdev;

typedef struct { uint64_t ino, size; uint32_t mode; uint32_t nlink; int64_t mtime_sec; int64_t atime_sec; int64_t ctime_sec; } usbmem_ntfs_stat;
typedef int (*usbmem_ntfs_dir_cb)(void *user, const char *name, int len, uint64_t ino, unsigned dtype);

#define USBMEM_NTFS_ROOT_INO 5u  /* FILE_root */

int usbmem_ntfs_mount(struct usbmem_bdev *bdev, int read_only, usbmem_ntfs **out);
void usbmem_ntfs_unmount(usbmem_ntfs *);
int usbmem_ntfs_lookup(usbmem_ntfs *, uint64_t dir_ino, const char *name, size_t len, uint64_t *ino);
int usbmem_ntfs_stat_ino(usbmem_ntfs *, uint64_t ino, usbmem_ntfs_stat *st);
int usbmem_ntfs_readdir(usbmem_ntfs *, uint64_t dir_ino, usbmem_ntfs_dir_cb cb, void *user);
int64_t usbmem_ntfs_read(usbmem_ntfs *, uint64_t ino, uint64_t off, void *buf, size_t len);

/* 書き込み (実験的。ビルドで USBMEM_NTFS_WRITE=ON、かつ usbmem_ntfs_mount(read_only=0) のときだけ有効。それ以外は -EROFS)。 */
int usbmem_ntfs_sync(usbmem_ntfs *);
/* 非常駐・非圧縮・非暗号の既存ファイルの [off, off+len) を上書き (サイズ拡張なし)。段階 2 の足場。 */
int64_t usbmem_ntfs_write_inplace(usbmem_ntfs *, uint64_t ino, uint64_t off, const void *buf, size_t len);

/* 上流の write_iter 経由の書き込み (拡張可)。書いたバイト数 / 負の errno。要 sync。 */
int64_t usbmem_ntfs_write(usbmem_ntfs *, uint64_t ino, uint64_t off, const void *buf, size_t len);
int usbmem_ntfs_writable(usbmem_ntfs *);   /* 1 = 書き込み可能 (rw 要求でも読み取り専用に落ちることがある) */
int usbmem_ntfs_truncate(usbmem_ntfs *, uint64_t ino, uint64_t size);   /* 実験的: 拡張・縮小 */

/* 名前空間の変更 (実験的)。いずれも書き込みマウント時のみ。dir_ino の直下の name を対象にする。 */
int usbmem_ntfs_create(usbmem_ntfs *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_ntfs_mkdir(usbmem_ntfs *, uint64_t dir_ino, const char *name, size_t len, uint32_t mode, uint64_t *ino);
int usbmem_ntfs_unlink(usbmem_ntfs *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_ntfs_rmdir(usbmem_ntfs *, uint64_t dir_ino, const char *name, size_t len);
int usbmem_ntfs_rename(usbmem_ntfs *, uint64_t old_dir, const char *old_name, size_t old_len, uint64_t new_dir, const char *new_name, size_t new_len);

/* BlockDevice ブリッジ。dev は usbmem::BlockDevicePtr を指す不透明ポインタ(C++ ラッパが渡す)。 */
struct usbmem_bdev *usbmem_bdev_wrap(void *shared_ptr_copy_owner);
void usbmem_bdev_free(struct usbmem_bdev *);

#ifdef __cplusplus
}
#endif
#endif
