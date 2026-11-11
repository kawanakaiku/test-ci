/* 自動生成: tools/gen_ops_structs.py (手編集しない) */
#ifndef USBMEM_COMPAT_OPS_H
#define USBMEM_COMPAT_OPS_H

struct address_space_operations {
  void *bmap;
  void *dirty_folio;
  void *error_remove_folio;
  void *invalidate_folio;
  void *is_partially_uptodate;
  void *migrate_folio;
  void *read_folio;
  void *readahead;
  void *release_folio;
  void *swap_activate;
  void *writepages;
};
struct dentry_operations {
  void *d_compare;
  void *d_delete;
  void *d_hash;
  void *d_revalidate;
};
struct export_operations {
  void *encode_fh;
  void *fh_to_dentry;
  void *fh_to_parent;
  void *get_parent;
};
struct file_operations {
  void *compat_ioctl;
  void *fallocate;
  void *fsync;
  void *iterate_shared;
  void *llseek;
  void *mmap_prepare;
  void *open;
  void *read;
  void *read_iter;
  void *release;
  void *setlease;
  void *splice_read;
  void *splice_write;
  void *unlocked_ioctl;
  void *write_iter;
};
struct fs_context_operations {
  void *free;
  void *get_tree;
  void *parse_param;
  void *reconfigure;
};
struct inode_operations {
  void *create;
  void *fiemap;
  void *fileattr_get;
  void *fileattr_set;
  void *get_acl;
  void *get_link;
  void *getattr;
  void *link;
  void *listxattr;
  void *lookup;
  void *mkdir;
  void *mknod;
  void *permission;
  void *rename;
  void *rmdir;
  void *set_acl;
  void *setattr;
  void *symlink;
  void *unlink;
  void *update_time;
};
struct super_operations {
  void *alloc_inode;
  void *drop_inode;
  void *evict_inode;
  void *free_inode;
  void *put_super;
  void *show_options;
  void *shutdown;
  void *statfs;
  void *sync_fs;
  void *write_inode;
};
struct vm_operations_struct {
  void *fault;
  void *map_pages;
  void *page_mkwrite;
};
struct xattr_handler {
  void *get;
  void *list;
  void *prefix;
  void *set;
};

#endif
