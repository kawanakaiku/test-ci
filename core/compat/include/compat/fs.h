/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VFS オブジェクトの最小定義。fs/ntfs が実際に参照するメンバのみを持つ
 * (third_party の grep 結果に基づく)。意味論は core/compat/src/ で実装する。
 */
#ifndef USBMEM_COMPAT_FS_H
#define USBMEM_COMPAT_FS_H
#include <compat/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 時刻 / uid */
struct timespec64 { s64 tv_sec; long tv_nsec; };
typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
typedef u32 errseq_t;
#ifdef __ANDROID__
#include <linux/posix_types.h>   /* bionic が定義済み (int val[2]) */
#else
typedef struct { u32 val[2]; } __kernel_fsid_t;
#endif
struct user_namespace;
struct mnt_idmap;
struct fs_struct;
struct workqueue_struct;
struct work_struct { void (*func)(struct work_struct *); struct work_struct *dnext_; int pending_; };   /* dnext_/pending_: 互換層の保留リスト用 (rt_core.c) */
struct delayed_call;
#define from_kuid_munged(ns, u) ((u).val)
#define from_kgid_munged(ns, g) ((g).val)
#define uid_valid(u) ((u).val != (uid_t)-1)
#define gid_valid(g) ((g).val != (gid_t)-1)
kuid_t make_kuid(struct user_namespace *ns, uid_t id);
kgid_t make_kgid(struct user_namespace *ns, gid_t id);

/* ---- NLS / UTF */
struct nls_table { const char *charset; int (*uni2char)(u16, unsigned char *, int); int (*char2uni)(const unsigned char *, int, void *); };

/* ---- page / folio */
struct address_space;
struct folio;
struct page { struct folio *folio_; };
struct folio {
  unsigned long flags;
  pgoff_t index;
  struct address_space *mapping;
  void *private;
  void *data;          /* PAGE_SIZE バイトのバッファ (ホスト側メモリ) */
  atomic_t refcount;
  struct mutex lock_;
  struct page page;
  struct folio *hash_next_;
  int read_pending_, read_err_;
  /* 512B 単位の dirty ビット (bit i = [512*i, 512*(i+1)))。実カーネルの iomap (iomap_folio_state) と同じく、書いた範囲だけを書き戻すために持つ。
   * クラスタ < 4096 では 1 folio の中にデータ・未初期化・穴が混在し、folio 全体の書き戻しは穴/未初期化の範囲で失敗する。
   * dirty_blocks_ = 次の書き戻し対象、wb_blocks_ = 書き戻し中に確定した対象 (失敗時は dirty_blocks_ へ戻す)。 */
  unsigned char dirty_blocks_, wb_blocks_;
};
enum { FOLIO_LOCKED, FOLIO_UPTODATE, FOLIO_DIRTY, FOLIO_WRITEBACK };

/* ---- ブロックデバイス / bio */
struct usbmem_bdev;  /* core の BlockDevice ブリッジ (src/bdev_bridge.cpp) */
struct address_space; struct block_device; struct file_system_type;
struct bio_vec { struct page *bv_page; unsigned bv_len, bv_offset; };
struct bvec_iter { sector_t bi_sector; unsigned bi_size; };
struct bio {
  struct bio_vec *bi_io_vec;
  unsigned short bi_vcnt, bi_max_vecs;
  struct bvec_iter bi_iter;
  struct block_device *bi_bdev;
  blk_opf_t bi_opf;
  int bi_status;
  void *bi_private;
  void (*bi_end_io)(struct bio *);
  int remaining_; unsigned pad_;   /* 互換層内部: bio_chain の残数 / bioset front_pad 分の確保オフセット */
};
struct bio_set { unsigned front_pad; };

/* ---- inode / super_block / dentry / file */
struct qstr { const unsigned char *name; unsigned int len; unsigned int hash; };
struct inode_operations; struct file_operations; struct address_space_operations;
struct super_operations; struct export_operations; struct dentry_operations; struct posix_acl;

struct address_space {
  struct inode *host;
  const struct address_space_operations *a_ops;
  struct rw_semaphore invalidate_lock;
  struct list_head private_list;
  struct block_device *host_bdev_;  /* bd_mapping の場合のみ */
  void *pages;  /* 実装依存: ページキャッシュ (src/pagecache.cpp) */
  unsigned long nrpages;
};
struct block_device { struct usbmem_bdev *impl; struct address_space *bd_mapping; struct address_space bd_data_; u32 bd_dev; };
struct inode {
  struct super_block *i_sb;
  struct address_space *i_mapping;
  struct address_space i_data;
  umode_t i_mode;
  unsigned int i_flags;
  unsigned long i_ino;
  kuid_t i_uid; kgid_t i_gid;
  unsigned int i_nlink;
  spinlock_t i_lock;
  loff_t i_size;
  unsigned long i_blocks;
  unsigned int i_blkbits;
  u32 i_generation;
  atomic_t i_count;
  const struct inode_operations *i_op;
  const struct file_operations *i_fop;
  struct posix_acl *i_acl, *i_default_acl;
  struct rw_semaphore i_rwsem;
  struct timespec64 i_atime_, i_mtime_, i_ctime_;
  u64 i_version;
  unsigned long i_state;
  struct inode *i_hash_next_;
  bool i_hashed_;
  unsigned long i_hashval_;
};
struct dentry {
  struct qstr d_name;
  struct inode *d_inode;
  struct dentry *d_parent;
  struct super_block *d_sb;
  void *d_fsdata;
};
struct super_block {
  struct block_device *s_bdev;
  unsigned long s_flags;
  unsigned long s_magic;
  unsigned long s_blocksize;
  unsigned char s_blocksize_bits;
  loff_t s_maxbytes;
  void *s_fs_info;
  struct dentry *s_root;
  char s_id[32];
  errseq_t s_wb_err;
  u32 s_time_gran;
  const struct super_operations *s_op;
  const struct export_operations *s_export_op;
  const void *s_xattr;
  struct backing_dev_info *s_bdi;
  struct file_system_type *s_type;   /* get_tree_bdev が設定。umount / fill_super 失敗時に kill_sb を呼ぶ (exfat は sbi をここで解放) */
  s64 s_time_min, s_time_max;
  int s_active_;
  struct fs_context *s_fc_;
};
struct vfsmount;
struct path { struct vfsmount *mnt; struct dentry *dentry; };
struct file {
  struct path f_path;
  struct address_space *f_mapping;
  fmode_t f_mode;
  unsigned int f_flags;
  loff_t f_pos;
  struct inode *f_inode;
  void *private_data;
};
struct kiocb { struct file *ki_filp; loff_t ki_pos; unsigned int ki_flags; };
struct iov_iter { const char *buf; size_t count; };   /* 単一の連続バッファのみ (ホスト API 用) */
struct kstat {
  u32 result_mask; umode_t mode; u64 attributes, attributes_mask;
  struct timespec64 atime, mtime, ctime;
  u32 blksize, dio_mem_align, dio_offset_align; u64 blocks, size;
  struct timespec64 btime;
};
typedef struct { uid_t val; } vfsuid_t;
typedef struct { gid_t val; } vfsgid_t;
struct iattr { unsigned int ia_valid; umode_t ia_mode; kuid_t ia_uid; kgid_t ia_gid; vfsuid_t ia_vfsuid; vfsgid_t ia_vfsgid; loff_t ia_size; struct timespec64 ia_atime, ia_mtime, ia_ctime; struct file *ia_file; };
struct kstatfs {
  long f_type, f_bsize; u64 f_blocks, f_bfree, f_bavail, f_ffree; __kernel_fsid_t f_fsid;
  long f_namelen, f_frsize; u64 f_files;
};
struct writeback_control { int sync_mode; pgoff_t *snap_; size_t nsnap_, pos_; int saved_err_; };

/* ---- iomap */
struct iomap {
  u64 addr; loff_t offset; u64 length; u16 type; u16 flags;
  struct block_device *bdev; void *inline_data; void *private;
};

/* ---- mount (fs_context) */
struct fs_context { unsigned int sb_flags; void *s_fs_info; struct dentry *root; unsigned int purpose; const void *ops; };
struct fs_parameter { const char *key; char *string; };

#ifdef __cplusplus
}
#endif
#endif
