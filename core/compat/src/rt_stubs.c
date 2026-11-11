// SPDX-License-Identifier: GPL-2.0-or-later
// 自動生成: tools/gen_compat_stubs.py (手編集しない)。未実装 API のスタブ。
#include <compat/kernel.h>
#include <compat/usbmem_ntfs.h>

int blkdev_issue_discard(struct block_device * a0, sector_t a1, sector_t a2, gfp_t a3) {
  usbmem_unimplemented("blkdev_issue_discard"); return (int)-ENOSYS;
}
int blkdev_issue_zeroout(struct block_device * a0, sector_t a1, sector_t a2, gfp_t a3, unsigned a4) {
  usbmem_unimplemented("blkdev_issue_zeroout"); return (int)-ENOSYS;
}
size_t copy_folio_from_iter_atomic(struct folio * a0, size_t a1, size_t a2, struct iov_iter * a3) {
  usbmem_unimplemented("copy_folio_from_iter_atomic"); return (size_t)-ENOSYS;
}
size_t fault_in_iov_iter_readable(const struct iov_iter * a0, size_t a1) {
  usbmem_unimplemented("fault_in_iov_iter_readable"); return (size_t)-ENOSYS;
}
void file_accessed(struct file * a0) {
  usbmem_unimplemented("file_accessed");
}
struct mnt_idmap * file_mnt_idmap(const struct file * a0) {
  usbmem_unimplemented("file_mnt_idmap"); return (struct mnt_idmap *)0;
}
int file_modified(struct file * a0) {
  usbmem_unimplemented("file_modified"); return (int)-ENOSYS;
}
int file_remove_privs(struct file * a0) {
  usbmem_unimplemented("file_remove_privs"); return (int)-ENOSYS;
}
int file_update_time(struct file * a0) {
  usbmem_unimplemented("file_update_time"); return (int)-ENOSYS;
}
void fileattr_fill_flags(struct file_kattr * a0, u32 a1) {
  usbmem_unimplemented("fileattr_fill_flags");
}
bool fileattr_has_fsx(const struct file_kattr * a0) {
  usbmem_unimplemented("fileattr_has_fsx"); return false;
}
vm_fault_t filemap_fault(struct vm_fault * a0) {
  usbmem_unimplemented("filemap_fault"); return (vm_fault_t)-ENOSYS;
}
vm_fault_t filemap_map_pages(struct vm_fault * a0, pgoff_t a1, pgoff_t a2) {
  usbmem_unimplemented("filemap_map_pages"); return (vm_fault_t)-ENOSYS;
}
int filemap_migrate_folio(struct address_space * a0, struct folio * a1, struct folio * a2, int a3) {
  usbmem_unimplemented("filemap_migrate_folio"); return (int)-ENOSYS;
}
ssize_t filemap_splice_read(struct file * a0, loff_t * a1, struct pipe_inode_info * a2, size_t a3, unsigned a4) {
  usbmem_unimplemented("filemap_splice_read"); return (ssize_t)-ENOSYS;
}
int generic_encode_ino32_fh(struct inode * a0, __u32 * a1, int * a2, struct inode * a3) {
  usbmem_unimplemented("generic_encode_ino32_fh"); return (int)-ENOSYS;
}
int generic_error_remove_folio(struct address_space * a0, struct folio * a1) {
  usbmem_unimplemented("generic_error_remove_folio"); return (int)-ENOSYS;
}
struct dentry * generic_fh_to_dentry(struct super_block * a0, struct fid * a1, int a2, int a3, struct inode *(*a4)(struct super_block *, u64, u32)) {
  usbmem_unimplemented("generic_fh_to_dentry"); return (struct dentry *)0;
}
struct dentry * generic_fh_to_parent(struct super_block * a0, struct fid * a1, int a2, int a3, struct inode *(*a4)(struct super_block *, u64, u32)) {
  usbmem_unimplemented("generic_fh_to_parent"); return (struct dentry *)0;
}
loff_t generic_file_llseek(struct file * a0, loff_t a1, int a2) {
  usbmem_unimplemented("generic_file_llseek"); return (loff_t)-ENOSYS;
}
loff_t generic_file_llseek_size(struct file * a0, loff_t a1, int a2, loff_t a3, loff_t a4) {
  usbmem_unimplemented("generic_file_llseek_size"); return (loff_t)-ENOSYS;
}
ssize_t generic_file_read_iter(struct kiocb * a0, struct iov_iter * a1) {
  usbmem_unimplemented("generic_file_read_iter"); return (ssize_t)-ENOSYS;
}
ssize_t generic_read_dir(struct file * a0, char * a1, size_t a2, loff_t * a3) {
  usbmem_unimplemented("generic_read_dir"); return (ssize_t)-ENOSYS;
}
int generic_setlease(struct file * a0, int a1, void ** a2, void ** a3) {
  usbmem_unimplemented("generic_setlease"); return (int)-ENOSYS;
}
int generic_update_time(struct inode * a0, enum fs_update_time a1, unsigned a2) {
  usbmem_unimplemented("generic_update_time"); return (int)-ENOSYS;
}
int generic_write_checks(struct kiocb * a0, struct iov_iter * a1) {
  usbmem_unimplemented("generic_write_checks"); return (int)-ENOSYS;
}
ssize_t generic_write_sync(struct kiocb * a0, ssize_t a1) {
  usbmem_unimplemented("generic_write_sync"); return (ssize_t)-ENOSYS;
}
int get_write_access(struct inode * a0) {
  usbmem_unimplemented("get_write_access"); return (int)-ENOSYS;
}
ssize_t iomap_dio_rw(struct kiocb * a0, struct iov_iter * a1, iomap_iter_next_fn a2, const struct iomap_dio_ops * a3, unsigned a4, void * a5, size_t a6) {
  usbmem_unimplemented("iomap_dio_rw"); return (ssize_t)-ENOSYS;
}
int iomap_fiemap(struct inode * a0, struct fiemap_extent_info * a1, u64 a2, u64 a3, iomap_iter_next_fn a4) {
  usbmem_unimplemented("iomap_fiemap"); return (int)-ENOSYS;
}
ssize_t iomap_file_buffered_write(struct kiocb * a0, struct iov_iter * a1, iomap_iter_next_fn a2, const struct iomap_write_ops * a3, void * a4) {
  usbmem_unimplemented("iomap_file_buffered_write"); return (ssize_t)-ENOSYS;
}
void iomap_invalidate_folio(struct folio * a0, size_t a1, size_t a2) {
  usbmem_unimplemented("iomap_invalidate_folio");
}
bool iomap_is_partially_uptodate(struct folio * a0, size_t a1, size_t a2) {
  usbmem_unimplemented("iomap_is_partially_uptodate"); return false;
}
vm_fault_t iomap_page_mkwrite(struct vm_fault * a0, iomap_iter_next_fn a1, void * a2) {
  usbmem_unimplemented("iomap_page_mkwrite"); return (vm_fault_t)-ENOSYS;
}
bool iomap_release_folio(struct folio * a0, gfp_t a1) {
  usbmem_unimplemented("iomap_release_folio"); return false;
}
loff_t iomap_seek_data(struct inode * a0, loff_t a1, iomap_iter_next_fn a2) {
  usbmem_unimplemented("iomap_seek_data"); return (loff_t)-ENOSYS;
}
loff_t iomap_seek_hole(struct inode * a0, loff_t a1, iomap_iter_next_fn a2) {
  usbmem_unimplemented("iomap_seek_hole"); return (loff_t)-ENOSYS;
}
int iomap_swapfile_activate(struct swap_info_struct * a0, struct file * a1, sector_t * a2, iomap_iter_next_fn a3) {
  usbmem_unimplemented("iomap_swapfile_activate"); return (int)-ENOSYS;
}
int iomap_zero_range(struct inode * a0, loff_t a1, loff_t a2, bool * a3, iomap_iter_next_fn a4, const struct iomap_write_ops * a5, void * a6) {
  usbmem_unimplemented("iomap_zero_range"); return (int)-ENOSYS;
}
ssize_t iter_file_splice_write(struct pipe_inode_info * a0, struct file * a1, loff_t * a2, size_t a3, unsigned a4) {
  usbmem_unimplemented("iter_file_splice_write"); return (ssize_t)-ENOSYS;
}
int kiocb_modified(struct kiocb * a0) {
  usbmem_unimplemented("kiocb_modified"); return (int)-ENOSYS;
}
void mnt_drop_write_file(struct file * a0) {
  usbmem_unimplemented("mnt_drop_write_file");
}
int mnt_want_write_file(struct file * a0) {
  usbmem_unimplemented("mnt_want_write_file"); return (int)-ENOSYS;
}
int notify_change(struct mnt_idmap * a0, struct dentry * a1, struct iattr * a2, struct inode ** a3) {
  usbmem_unimplemented("notify_change"); return (int)-ENOSYS;
}
void put_write_access(struct inode * a0) {
  usbmem_unimplemented("put_write_access");
}
int rw_verify_area(int a0, struct file * a1, const loff_t * a2, size_t a3) {
  usbmem_unimplemented("rw_verify_area"); return (int)-ENOSYS;
}
void setattr_copy(struct mnt_idmap * a0, struct inode * a1, const struct iattr * a2) {
  usbmem_unimplemented("setattr_copy");
}
int setattr_prepare(struct mnt_idmap * a0, struct dentry * a1, struct iattr * a2) {
  usbmem_unimplemented("setattr_prepare"); return (int)-ENOSYS;
}
void simple_rename_timestamp(struct inode * a0, struct dentry * a1, struct inode * a2, struct dentry * a3) {
  usbmem_unimplemented("simple_rename_timestamp");
}
char * strndup_user(const char * a0, long a1) {
  usbmem_unimplemented("strndup_user"); return (char *)0;
}
void super_set_uuid(struct super_block * a0, const u8 * a1, unsigned a2) {
  usbmem_unimplemented("super_set_uuid");
}
int vma_desc_test_all(const void * a0, int a1, int a2) {
  usbmem_unimplemented("vma_desc_test_all"); return (int)-ENOSYS;
}
void * vmap(struct page ** a0, unsigned a1, unsigned long a2, int a3) {
  usbmem_unimplemented("vmap"); return (void *)0;
}
void * vmemdup_user(const void * a0, size_t a1) {
  usbmem_unimplemented("vmemdup_user"); return (void *)0;
}
void vunmap(const void * a0) {
  usbmem_unimplemented("vunmap");
}
