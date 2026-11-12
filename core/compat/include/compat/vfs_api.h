/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VFS/MM/ブロック層 API の宣言。実装は core/compat/src/ 。
 * 読み取り専用段階で使わないものは宣言のみ (リンクが必要になった時点で実装 or -ENOSYS スタブ)。
 */
#ifndef USBMEM_COMPAT_VFS_API_H
#define USBMEM_COMPAT_VFS_API_H
#include <compat/fs.h>
#include <compat/consts.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rb_node { unsigned long __rb_parent_color; struct rb_node *rb_right, *rb_left; };
struct rb_root { struct rb_node *rb_node; };
struct fid; struct pipe_inode_info; struct vm_fault_; struct folio_batch; struct vm_fault; struct vm_area_struct { struct file *vm_file; }; struct fiemap_extent_info;
struct swap_info_struct; struct seq_file; struct file_ra_state; struct readahead_control;
struct fs_parse_result { bool negated; union { bool boolean; int int_32; unsigned uint_32; u64 uint_64; }; kuid_t uid; kgid_t gid; };
struct fs_parameter_spec; struct constant_table { const char *name; int value; };
struct file_kattr { u32 flags; u32 fsx_xflags; };
struct dir_context;
typedef bool (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
struct dir_context { filldir_t actor; loff_t pos; void *user; };
struct delayed_call { void (*fn)(void *); void *arg; };
struct pt_regs; struct poll_table_struct; struct kobject; struct xattr_handler;
struct vfsmount;
struct writeback_iter_ctx; struct bio_set;

#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define RB_EMPTY_ROOT(root) ((root)->rb_node == NULL)
void rb_insert_color(struct rb_node *, struct rb_root *);
void rb_erase(struct rb_node *, struct rb_root *);
struct rb_node *rb_first(const struct rb_root *);
static inline void rb_link_node(struct rb_node *n, struct rb_node *parent, struct rb_node **link) {
  n->__rb_parent_color = (unsigned long)parent; n->rb_left = n->rb_right = NULL; *link = n;
}

/* ---- 時刻/ inode アクセサ */
#define ktime_get_coarse_real_ts64(ts) usbmem_now(ts)
void usbmem_now(struct timespec64 *ts);
struct timespec64 current_time(struct inode *);
static inline loff_t i_size_read(const struct inode *i) { return i->i_size; }
static inline void i_size_write(struct inode *i, loff_t s) { i->i_size = s; }
static inline uid_t i_uid_read(const struct inode *i) { return i->i_uid.val; }
static inline gid_t i_gid_read(const struct inode *i) { return i->i_gid.val; }
static inline void i_uid_write(struct inode *i, uid_t u) { i->i_uid.val = u; }
static inline void i_gid_write(struct inode *i, gid_t g) { i->i_gid.val = g; }
static inline struct timespec64 inode_get_atime(const struct inode *i) { return i->i_atime_; }
static inline struct timespec64 inode_get_mtime(const struct inode *i) { return i->i_mtime_; }
static inline struct timespec64 inode_get_ctime(const struct inode *i) { return i->i_ctime_; }
static inline struct timespec64 inode_set_atime_to_ts(struct inode *i, struct timespec64 t) { return i->i_atime_ = t; }
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *i, struct timespec64 t) { return i->i_mtime_ = t; }
static inline struct timespec64 inode_set_ctime_to_ts(struct inode *i, struct timespec64 t) { return i->i_ctime_ = t; }
struct timespec64 inode_set_ctime_current(struct inode *);
#define inode_inc_iversion(i) (++(i)->i_version)
#define inode_set_iversion(i, v) ((i)->i_version = (v))
static inline void set_nlink(struct inode *i, unsigned n) { i->i_nlink = n; }
static inline void inc_nlink(struct inode *i) { i->i_nlink++; }
static inline void drop_nlink(struct inode *i) { i->i_nlink--; }
static inline void clear_nlink(struct inode *i) { i->i_nlink = 0; }
static inline void inode_set_flags(struct inode *i, unsigned flags, unsigned mask) { i->i_flags = (i->i_flags & ~mask) | flags; }
static inline unsigned i_blocksize(const struct inode *i) { return 1u << i->i_blkbits; }
#define S_ISDIR_(m) S_ISDIR(m)
#define IS_ERR_OR_NULL_ IS_ERR_OR_NULL
static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
static inline struct inode *d_inode(const struct dentry *d) { return d->d_inode; }
static inline int sb_rdonly(const struct super_block *sb) { return sb->s_flags & SB_RDONLY; }
#define inode_state_read_once(i) ((i)->i_state)
#define inode_state_set(i, f) ((i)->i_state |= (f))

/* ---- inode キャッシュ / ライフサイクル */
struct inode *iget5_locked(struct super_block *, unsigned long, int (*test)(struct inode *, void *),
                           int (*set)(struct inode *, void *), void *);
struct inode *ilookup5(struct super_block *, unsigned long, int (*test)(struct inode *, void *), void *);
struct inode *find_inode_nowait(struct super_block *, unsigned long,
                                int (*match)(struct inode *, u64, void *), void *);
struct inode *find_inode_rcu(struct super_block *, unsigned long, int (*test)(struct inode *, void *), void *);
struct inode *new_inode(struct super_block *);
void *alloc_inode_sb(struct super_block *, struct kmem_cache *, gfp_t);
void unlock_new_inode(struct inode *);
void discard_new_inode(struct inode *);
void iput(struct inode *);
struct inode *igrab(struct inode *);
void ihold(struct inode *);
void clear_inode(struct inode *);
void inode_init_once(struct inode *);
void insert_inode_hash(struct inode *);
void remove_inode_hash(struct inode *);
int inode_unhashed(struct inode *);
void mark_inode_dirty(struct inode *);
void mark_inode_dirty_sync(struct inode *);
void __mark_inode_dirty(struct inode *, int);
int write_inode_now(struct inode *, int);
void inode_attach_wb(struct inode *, struct folio *);
void init_special_inode(struct inode *, umode_t, dev_t);
void inode_init_owner(struct mnt_idmap *, struct inode *, const struct inode *, umode_t);
void inode_dio_wait(struct inode *);
int inode_generic_drop(struct inode *);
int always_delete_dentry(const struct dentry *);
#define inode_lock(i) down_write(&(i)->i_rwsem)
#define inode_unlock(i) up_write(&(i)->i_rwsem)
#define inode_lock_shared(i) down_read(&(i)->i_rwsem)
#define inode_unlock_shared(i) up_read(&(i)->i_rwsem)
#define inode_lock_nested(i, s) down_write(&(i)->i_rwsem)
#define inode_trylock(i) (pthread_rwlock_trywrlock(&(i)->i_rwsem.l) == 0)

/* ---- dentry */
struct dentry *d_make_root(struct inode *);
struct dentry *d_obtain_alias(struct inode *);
struct dentry *d_find_alias(struct inode *);
struct dentry *d_splice_alias(struct inode *, struct dentry *);
struct dentry *d_add_ci(struct dentry *, struct inode *, struct qstr *);
void d_instantiate(struct dentry *, struct inode *);
void d_instantiate_new(struct dentry *, struct inode *);
void dput(struct dentry *);
struct inode *d_backing_inode(const struct dentry *);
char *dentry_path_raw(const struct dentry *, char *, int);
unsigned int full_name_hash(const void *salt, const char *, unsigned int);
int is_subdir(struct dentry *, struct dentry *);
void set_default_d_op(struct super_block *, const struct dentry_operations *);
bool dir_emit(struct dir_context *, const char *, int, u64, unsigned);
bool dir_emit_dots(struct file *, struct dir_context *);

/* ---- ページキャッシュ / folio */
void *folio_address(const struct folio *);
size_t folio_size(const struct folio *);
unsigned int folio_shift(const struct folio *);
long folio_nr_pages(const struct folio *);
loff_t folio_pos(const struct folio *);
loff_t folio_next_pos(const struct folio *);
bool folio_contains(const struct folio *, pgoff_t);
struct page *folio_page(struct folio *, unsigned long);
struct folio *page_folio(struct page *);
void folio_get(struct folio *);
void folio_put(struct folio *);
void folio_lock(struct folio *);
void folio_unlock(struct folio *);
int folio_test_locked(const struct folio *);
int folio_test_uptodate(const struct folio *);
int folio_test_writeback(const struct folio *);
void folio_mark_uptodate(struct folio *);
void folio_clear_uptodate(struct folio *);
void folio_mark_dirty(struct folio *);
/* folio の [off, off+len) だけを dirty にする (512B 単位に外側へ丸める)。folio_mark_dirty / iomap_dirty_folio は folio 全体 */
void usbmem_folio_dirty_range(struct folio *, size_t off, size_t len);
/* 逆: [off, off+len) に完全に含まれる 512B ブロックの dirty を外す (bforget / 直接書き込み済み) */
void usbmem_folio_clean_range(struct folio *, size_t off, size_t len);
void folio_clear_dirty(struct folio *);
void folio_start_writeback(struct folio *);
void folio_end_writeback(struct folio *);
void folio_wait_writeback(struct folio *);
void folio_redirty_for_writepage(struct writeback_control *, struct folio *);
void folio_zero_segment(struct folio *, size_t start, size_t end);
void folio_zero_segments(struct folio *, size_t s1, size_t e1, size_t s2, size_t e2);
void folio_fill_tail(struct folio *, size_t off, const char *from, size_t len);
/* カーネルと同じく、ファイル内オフセットでもメモリポインタでも「folio 内のオフセット」を返す (folio のデータは PAGE_SIZE 境界に置く)。 */
#define offset_in_folio(f, p) ((size_t)((unsigned long)(p) & (PAGE_SIZE - 1)))
#define offset_in_page(p) ((unsigned long)(p) & (PAGE_SIZE - 1))
#define page_offset(p) ((loff_t)(p)->folio_->index << PAGE_SHIFT)
void *kmap_local_folio(struct folio *, size_t off);
void *kmap_local_page(struct page *);
#define kunmap_local(p) ((void)(p))
void flush_dcache_folio(struct folio *);
void flush_dcache_page(struct page *);
void memcpy_from_folio(char *to, struct folio *, size_t off, size_t len);
void memcpy_to_folio(struct folio *, size_t off, const char *from, size_t len);
void memcpy_from_page(char *to, struct page *, size_t off, size_t len);
void memcpy_to_page(struct page *, size_t off, const char *from, size_t len);
void memzero_page(struct page *, size_t off, size_t len);
void zero_user_segment(struct page *, unsigned start, unsigned end);
void zero_user_segments(struct page *, unsigned, unsigned, unsigned, unsigned);
struct page *alloc_page(gfp_t);
void put_page(struct page *);
void lock_page(struct page *);
void unlock_page(struct page *);
int PageUptodate(const struct page *);
int PageDirty(const struct page *);
void SetPageUptodate(struct page *);

struct folio *read_mapping_folio(struct address_space *, pgoff_t, struct file *);
struct page *read_mapping_page(struct address_space *, pgoff_t, struct file *);
struct folio *filemap_grab_folio(struct address_space *, pgoff_t);
struct folio *__filemap_get_folio(struct address_space *, pgoff_t, unsigned fgp, gfp_t);
struct page *grab_cache_page_nowait(struct address_space *, pgoff_t);
gfp_t mapping_gfp_mask(struct address_space *);
void mapping_set_error(struct address_space *, int);
int filemap_write_and_wait(struct address_space *);
int filemap_write_and_wait_range(struct address_space *, loff_t, loff_t);
int file_write_and_wait_range(struct file *, loff_t, loff_t);
void filemap_invalidate_lock(struct address_space *);
void filemap_invalidate_unlock(struct address_space *);
void filemap_invalidate_lock_shared(struct address_space *);
void filemap_invalidate_unlock_shared(struct address_space *);
void truncate_inode_pages(struct address_space *, loff_t);
void truncate_inode_pages_final(struct address_space *);
void truncate_pagecache(struct inode *, loff_t);
void truncate_pagecache_range(struct inode *, loff_t, loff_t);
unsigned long invalidate_mapping_pages(struct address_space *, pgoff_t, pgoff_t);
void unmap_mapping_range(struct address_space *, loff_t, loff_t, int);
void pagecache_isize_extended(struct inode *, loff_t, loff_t);
void invalidate_kernel_vmap_range(void *, int);
void *vmap(struct page **, unsigned, unsigned long, int);
void vunmap(const void *);
void page_cache_sync_readahead(struct address_space *, struct file_ra_state *, struct file *, pgoff_t, unsigned long);
void file_ra_state_init(struct file_ra_state *, struct address_space *);
vm_fault_t vmf_error(int);
int errseq_check(errseq_t *, errseq_t);

/* ---- ブロック層 */
unsigned bdev_logical_block_size(struct block_device *);
u64 bdev_nr_bytes(struct block_device *);
unsigned bdev_discard_granularity(struct block_device *);
unsigned bdev_max_discard_sectors(struct block_device *);
int bdev_freeze(struct block_device *);
int bdev_thaw(struct block_device *);
int bdev_rw_virt(struct block_device *, sector_t, void *, size_t, blk_opf_t);
int sb_set_blocksize(struct super_block *, int);
int sb_min_blocksize(struct super_block *, int);
int sync_blockdev(struct block_device *);
int sync_filesystem(struct super_block *);
void sync_inodes_sb(struct super_block *);
int blkdev_issue_discard(struct block_device *, sector_t, sector_t, gfp_t);
int blkdev_issue_flush(struct block_device *);
int blkdev_issue_zeroout(struct block_device *, sector_t, sector_t, gfp_t, unsigned);
struct bio *bio_alloc(struct block_device *, unsigned short, blk_opf_t, gfp_t);
struct bio *bio_alloc_bioset(struct block_device *, unsigned short, blk_opf_t, gfp_t, struct bio_set *);
void bio_init(struct bio *, struct block_device *, struct bio_vec *, unsigned short, blk_opf_t);
void bio_uninit(struct bio *);
void bio_put(struct bio *);
void bio_chain(struct bio *, struct bio *);
int bio_add_page(struct bio *, struct page *, unsigned len, unsigned off);
bool bio_add_folio(struct bio *, struct folio *, size_t len, size_t off);
unsigned bio_add_vmalloc_chunk(struct bio *, void *, unsigned);
unsigned short bio_max_segs(unsigned);
sector_t bio_end_sector(const struct bio *);
int bioset_init(struct bio_set *, unsigned, unsigned, int);
void bioset_exit(struct bio_set *);
void submit_bio(struct bio *);
int submit_bio_wait(struct bio *);
int blk_status_to_errno(int);
#define bio_for_each_folio_all(fi, bio) for ((fi).idx_ = -1, usbmem_fi_next(&(fi), (bio)); (fi).folio; usbmem_fi_next(&(fi), (bio)))

/* ---- iomap (読み取り専用段階の最小実装) */
struct iomap_iter {
  struct inode *inode; loff_t pos; u64 len; loff_t iter_start_pos; int status; unsigned flags;
  struct iomap iomap; struct iomap srcmap; struct folio_batch *fbatch; void *private;
};
typedef int (*iomap_iter_begin_fn)(struct inode *inode, loff_t pos, loff_t length, unsigned flags,
                                   struct iomap *iomap, struct iomap *srcmap);
typedef int (*iomap_iter_end_fn)(struct inode *inode, loff_t pos, loff_t length, ssize_t written,
                                 unsigned flags, struct iomap *iomap);
typedef int (*iomap_iter_next_fn)(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap);
int iomap_iter_advance(struct iomap_iter *iter, u64 count);
int iomap_iter_continue(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap, int ret);
static inline u64 iomap_length_trim(const struct iomap_iter *iter, loff_t pos, u64 len) {
  u64 end = iter->iomap.offset + iter->iomap.length;
  if (iter->srcmap.type != IOMAP_HOLE) end = min(end, iter->srcmap.offset + iter->srcmap.length);
  return min(len, end - pos);
}
static inline u64 iomap_length(const struct iomap_iter *iter) { return iomap_length_trim(iter, iter->pos, iter->len); }
static inline int iomap_iter_advance_full(struct iomap_iter *iter) { return iomap_iter_advance(iter, iomap_length(iter)); }
static inline const struct iomap *iomap_iter_srcmap(const struct iomap_iter *i) {
  return i->srcmap.type != IOMAP_HOLE ? &i->srcmap : &i->iomap;
}
/* 上流 include/linux/iomap.h (GPL-2.0) の iomap_iter_next と同等 */
static __always_inline int iomap_iter_next(const struct iomap_iter *iter, struct iomap *iomap,
                                           struct iomap *srcmap, iomap_iter_begin_fn begin,
                                           iomap_iter_end_fn end) {
  int ret = 0;
  if (iomap->length) {
    if (end) {
      ssize_t advanced = iter->pos - iter->iter_start_pos;
      loff_t len = iomap_length_trim(iter, iter->iter_start_pos, iter->len + advanced);
      ret = end(iter->inode, iter->iter_start_pos, len, advanced, iter->flags, iomap);
    }
    ret = iomap_iter_continue(iter, iomap, srcmap, ret);
    if (ret <= 0) return ret;
  }
  ret = begin(iter->inode, iter->pos, iter->len, iter->flags, iomap, srcmap);
  return ret < 0 ? ret : 1;
}
#define DEFINE_IOMAP_ITER_NEXT_END(name, begin_fn, end_fn) \
  int name(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap) { \
    return iomap_iter_next(iter, iomap, srcmap, begin_fn, end_fn); }
#define DEFINE_IOMAP_ITER_NEXT(name, begin_fn) DEFINE_IOMAP_ITER_NEXT_END(name, begin_fn, NULL)
#define DECLARE_IOMAP_ITER_NEXT(name) int name(const struct iomap_iter *iter, struct iomap *iomap, struct iomap *srcmap)
struct folio;
struct iomap_write_ops {
  struct folio *(*get_folio)(struct iomap_iter *iter, loff_t pos, unsigned len);
  void (*put_folio)(struct inode *inode, loff_t pos, unsigned copied, struct folio *folio);
  bool (*iomap_valid)(struct inode *inode, const struct iomap *iomap);
  int (*read_folio_range)(const struct iomap_iter *iter, struct folio *folio, loff_t pos, size_t len);
};

struct iomap_writepage_ctx;
struct iomap_writeback_ops;
struct iomap_read_folio_ctx;
struct iomap_read_ops {
  int (*read_folio_range)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t len);
  void (*submit_read)(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx);
  struct bio_set *bio_set;
};
struct iomap_read_folio_ctx {
  const struct iomap_read_ops *ops; struct folio *cur_folio; struct readahead_control *rac;
  void *read_ctx; loff_t read_ctx_file_offset; void *vi;
};
struct iomap_dio_ops {
  int (*end_io)(struct kiocb *iocb, ssize_t size, int error, unsigned flags);
  void (*submit_io)(const struct iomap_iter *iter, struct bio *bio, loff_t file_offset);
  struct bio_set *bio_set;
};
typedef void (*bio_end_io_t)(struct bio *);
struct folio_iter { struct folio *folio; size_t offset; size_t length; int idx_; };
void usbmem_fi_next(struct folio_iter *, struct bio *);
struct va_format { const char *fmt; va_list *va; };
struct iomap_ioend { void *io_private; struct inode *io_inode; loff_t io_offset; size_t io_size; struct bio io_bio; unsigned io_flags; int io_error; };
extern const struct iomap_read_ops iomap_bio_read_ops;
void iomap_read_folio(iomap_iter_next_fn, struct iomap_read_folio_ctx *, void *private);
void iomap_readahead(iomap_iter_next_fn, struct iomap_read_folio_ctx *, void *private);
bool iomap_dirty_folio(struct address_space *, struct folio *);
ssize_t iomap_file_buffered_write(struct kiocb *, struct iov_iter *, iomap_iter_next_fn, const struct iomap_write_ops *, void *);
int iomap_zero_range(struct inode *, loff_t, loff_t, bool *, iomap_iter_next_fn, const struct iomap_write_ops *, void *);
vm_fault_t iomap_page_mkwrite(struct vm_fault *, iomap_iter_next_fn, void *);
int iomap_fiemap(struct inode *, struct fiemap_extent_info *, u64, u64, iomap_iter_next_fn);
loff_t iomap_seek_hole(struct inode *, loff_t, iomap_iter_next_fn);
loff_t iomap_seek_data(struct inode *, loff_t, iomap_iter_next_fn);
int iomap_swapfile_activate(struct swap_info_struct *, struct file *, sector_t *, iomap_iter_next_fn);
int iomap_writepages(struct iomap_writepage_ctx *);
ssize_t iomap_dio_rw(struct kiocb *, struct iov_iter *, iomap_iter_next_fn, const struct iomap_dio_ops *, unsigned, void *, size_t);
void iomap_invalidate_folio(struct folio *, size_t, size_t);
bool iomap_release_folio(struct folio *, gfp_t);
bool iomap_is_partially_uptodate(struct folio *, size_t, size_t);
int iomap_bio_read_folio_range(const struct iomap_iter *, struct iomap_read_folio_ctx *, size_t);
void iomap_bio_submit_read_endio(const struct iomap_iter *, struct iomap_read_folio_ctx *, bio_end_io_t);
int iomap_add_to_ioend(struct iomap_writepage_ctx *, struct folio *, loff_t, loff_t, unsigned);
struct iomap_writeback_ops {
  ssize_t (*writeback_range)(struct iomap_writepage_ctx *, struct folio *, u64, unsigned, u64);
  int (*writeback_submit)(struct iomap_writepage_ctx *, int);
};
struct iomap_writepage_ctx { struct iomap iomap; struct inode *inode; struct writeback_control *wbc; const struct iomap_writeback_ops *ops; u32 nr_folios; void *wb_ctx; };
int iomap_ioend_writeback_submit(struct iomap_writepage_ctx *, int);
enum fs_update_time { FS_UPD_ATIME, FS_UPD_CMTIME };

/* ---- file / uio / VFS 操作 (書き込み・VFS 公開面。宣言のみ) */
size_t iov_iter_count(const struct iov_iter *);
size_t copy_folio_from_iter_atomic(struct folio *, size_t, size_t, struct iov_iter *);
size_t fault_in_iov_iter_readable(const struct iov_iter *, size_t);
#define copy_from_user(to, from, n) (memcpy(to, from, n), 0UL)
#define copy_to_user(to, from, n) (memcpy(to, from, n), 0UL)
#define get_user(x, p) ((x) = *(p), 0)
void *vmemdup_user(const void *, size_t);
char *strndup_user(const char *, long);
int fatal_signal_pending_(void);
#define fatal_signal_pending(t) 0
struct io_context;
extern struct task_struct { int unused; struct fs_struct *fs; int pid; char comm[16]; void *journal_info; unsigned int flags; struct io_context *io_context; } *current;
bool capable(int);
int inode_permission(struct mnt_idmap *, struct inode *, int);
bool inode_owner_or_capable(struct mnt_idmap *, const struct inode *);
int setattr_prepare(struct mnt_idmap *, struct dentry *, struct iattr *);
void setattr_copy(struct mnt_idmap *, struct inode *, const struct iattr *);
int inode_newsize_ok(const struct inode *, loff_t);
int notify_change(struct mnt_idmap *, struct dentry *, struct iattr *, struct inode **);
void generic_fillattr(struct mnt_idmap *, u32, struct inode *, struct kstat *);
int generic_update_time(struct inode *, enum fs_update_time, unsigned);
int generic_write_checks(struct kiocb *, struct iov_iter *);
ssize_t generic_write_sync(struct kiocb *, ssize_t);
ssize_t generic_file_read_iter(struct kiocb *, struct iov_iter *);
loff_t generic_file_llseek(struct file *, loff_t, int);
loff_t generic_file_llseek_size(struct file *, loff_t, int, loff_t, loff_t);
int generic_file_open(struct inode *, struct file *);
ssize_t generic_read_dir(struct file *, char *, size_t, loff_t *);
int generic_setlease(struct file *, int, void **, void **);
int generic_error_remove_folio(struct address_space *, struct folio *);
struct dentry *generic_fh_to_dentry(struct super_block *, struct fid *, int, int, struct inode *(*)(struct super_block *, u64, u32));
struct dentry *generic_fh_to_parent(struct super_block *, struct fid *, int, int, struct inode *(*)(struct super_block *, u64, u32));
int generic_encode_ino32_fh(struct inode *, __u32 *, int *, struct inode *);
ssize_t filemap_splice_read(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned);
ssize_t iter_file_splice_write(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned);
vm_fault_t filemap_fault(struct vm_fault *);
vm_fault_t filemap_map_pages(struct vm_fault *, pgoff_t, pgoff_t);
int filemap_migrate_folio(struct address_space *, struct folio *, struct folio *, int);
int file_modified(struct file *);
int kiocb_modified(struct kiocb *);
void file_accessed(struct file *);
int file_update_time(struct file *);
int file_remove_privs(struct file *);
int rw_verify_area(int, struct file *, const loff_t *, size_t);
int get_write_access(struct inode *);
void put_write_access(struct inode *);
int mnt_want_write_file(struct file *);
void mnt_drop_write_file(struct file *);
struct mnt_idmap *file_mnt_idmap(const struct file *);
loff_t vfs_setpos(struct file *, loff_t, loff_t);
void fileattr_fill_flags(struct file_kattr *, u32);
bool fileattr_has_fsx(const struct file_kattr *);
int posix_acl_chmod(struct mnt_idmap *, struct dentry *, umode_t);
struct timespec64 simple_inode_init_ts(struct inode *);
void simple_rename_timestamp(struct inode *, struct dentry *, struct inode *, struct dentry *);
void set_delayed_call(struct delayed_call *, void (*)(void *), void *);
void kfree_link(void *);
void sb_start_intwrite(struct super_block *);
void sb_end_intwrite(struct super_block *);
void sb_start_pagefault(struct super_block *);
void sb_end_pagefault(struct super_block *);
void super_set_uuid(struct super_block *, const u8 *, unsigned);
__kernel_fsid_t u64_to_fsid(u64);
int vma_desc_test_all(const void *, int, int);

/* ---- mount / module / seq / workqueue / NLS */
int register_filesystem(void *);
int unregister_filesystem(void *);
int get_tree_bdev(struct fs_context *, int (*fill_super)(struct super_block *, struct fs_context *));
void kill_block_super(struct super_block *);
int fs_parse(struct fs_context *, const struct fs_parameter_spec *, struct fs_parameter *, struct fs_parse_result *);
void rcu_barrier(void);
struct workqueue_struct *alloc_workqueue(const char *, unsigned, int, ...);
void destroy_workqueue(struct workqueue_struct *);
bool queue_work(struct workqueue_struct *, struct work_struct *);
bool cancel_work_sync(struct work_struct *);
#define INIT_WORK(w, f) ((w)->func = (f), (w)->pending_ = 0, (w)->dnext_ = 0)
void seq_printf(struct seq_file *, const char *, ...);
void seq_puts(struct seq_file *, const char *);
int register_nls(struct nls_table *);
int unregister_nls(struct nls_table *);
struct nls_table *load_nls(const char *);
struct nls_table *load_nls_default(void);
void unload_nls(struct nls_table *);
int utf16s_to_utf8s(const void *, int, int endian, u8 *, int);
int utf8s_to_utf16s(const u8 *, int, int endian, void *, int);

#ifdef __cplusplus
}
#endif
#endif
