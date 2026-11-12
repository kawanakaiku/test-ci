/* 自動生成: tools/gen_ops_structs.py (手編集しない) */
#ifndef USBMEM_COMPAT_OPS_H
#define USBMEM_COMPAT_OPS_H

struct address_space_operations {
  void *bmap;
  void *direct_IO;
  void *dirty_folio;
  void *error_remove_folio;
  void *invalidate_folio;
  void *is_partially_uptodate;
  void *migrate_folio;
  void *read_folio;
  void *readahead;
  void *release_folio;
  void *swap_activate;
  void *write_begin;
  void *write_end;
  void *writepages;
};
struct dentry_operations {
  void *d_compare;
  void *d_delete;
  void *d_hash;
  void *d_revalidate;
};
struct dquot_operations {
  void *acquire_dquot;
  void *alloc_dquot;
  void *destroy_dquot;
  void *get_inode_usage;
  void *get_next_id;
  void *get_projid;
  void *get_reserved_space;
  void *mark_dirty;
  void *release_dquot;
  void *write_dquot;
  void *write_info;
};
struct export_operations {
  void *commit_metadata;
  void *encode_fh;
  void *fh_to_dentry;
  void *fh_to_parent;
  void *get_parent;
};
struct file_operations {
  void *compat_ioctl;
  void *fallocate;
  void *fop_flags;
  void *fsync;
  void *get_unmapped_area;
  void *iopoll;
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
  void *get_inode_acl;
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
  void *tmpfile;
  void *unlink;
  void *update_time;
};
struct super_operations {
  void *alloc_inode;
  void *destroy_inode;
  void *dirty_inode;
  void *drop_inode;
  void *evict_inode;
  void *free_inode;
  void *freeze_fs;
  void *get_dquots;
  void *put_super;
  void *quota_read;
  void *quota_write;
  void *show_options;
  void *shutdown;
  void *statfs;
  void *sync_fs;
  void *sync_inode_metadata;
  void *unfreeze_fs;
  void *write_inode;
};
struct vm_operations_struct {
  void *fault;
  void *huge_fault;
  void *map_pages;
  void *page_mkwrite;
  void *pfn_mkwrite;
};
struct xattr_handler {
  void *get;
  void *list;
  void *prefix;
  void *set;
};

#endif
