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

/* BlockDevice ブリッジ。dev は usbmem::BlockDevicePtr を指す不透明ポインタ(C++ ラッパが渡す)。 */
struct usbmem_bdev *usbmem_bdev_wrap(void *shared_ptr_copy_owner);
void usbmem_bdev_free(struct usbmem_bdev *);

#ifdef __cplusplus
}
#endif
#endif
