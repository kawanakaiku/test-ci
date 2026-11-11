/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LinuxCompat が提供する「ホスト側から VFS 風に使う」入口 (C)。実装: src/*.c */
#ifndef USBMEM_COMPAT_USBMEM_NTFS_H
#define USBMEM_COMPAT_USBMEM_NTFS_H
#include <compat/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

int usbmem_module_init_ntfs(void);
int usbmem_module_init_exfat(void);
void usbmem_unimplemented(const char *fn);
struct dentry *usbmem_d_alloc(struct super_block *, struct dentry *parent, const char *name, unsigned len);
void usbmem_bdev_mapping_init(struct block_device *);
void usbmem_bdev_mapping_free(struct block_device *);
void usbmem_d_free(struct dentry *);
void usbmem_compat_umount(struct super_block *);
int usbmem_compat_sync(struct super_block *);   /* 書き込み: dirty inode/folio を全てデバイスへ書き flush */
int usbmem_mapping_writeback(struct address_space *, int sync_all);
int usbmem_mapping_has_dirty(struct address_space *);
int usbmem_compat_mount(struct block_device *bdev, int read_only, struct super_block **out);   /* fs/ntfs (rt_vfs.c) */
int usbmem_compat_mount_fs(int (*module_init)(void), struct block_device *bdev, int read_only, struct super_block **out);

/* BlockDevice ブリッジ (src/bdev_bridge.cpp) */

int usbmem_bdev_read(struct usbmem_bdev *, u64 off, void *buf, size_t len);
int usbmem_bdev_write(struct usbmem_bdev *, u64 off, const void *buf, size_t len);
int usbmem_bdev_flush(struct usbmem_bdev *);
u64 usbmem_bdev_size(struct usbmem_bdev *);
unsigned usbmem_bdev_lbs(struct usbmem_bdev *);

#ifdef __cplusplus
}
#endif
#endif
