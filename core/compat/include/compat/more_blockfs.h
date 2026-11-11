/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * buffer_head 系ファイルシステム (fs/ext2 など) のために足した VFS 周辺の API。docs/ext2-port.md 参照。
 * 読み取りに使うものは src/rt_buffer.c などで実装し、書き込み専用のものは -ENOSYS / 何もしないスタブ (読み取り専用マウントでは呼ばれない)。
 */
#ifndef USBMEM_COMPAT_MORE_BLOCKFS_H
#define USBMEM_COMPAT_MORE_BLOCKFS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef s64 time64_t;
void usbmem_unimplemented(const char *fn);
struct usbmem_bdev; u64 usbmem_bdev_size(struct usbmem_bdev *);
struct rb_node *rb_next(const struct rb_node *);
struct rb_node *rb_prev(const struct rb_node *);
#define ext2_set_bit_atomic(lock, nr, addr) ext2_set_bit((nr), (addr))
#define ext2_clear_bit_atomic(lock, nr, addr) ext2_clear_bit((nr), (addr))

struct buffer_head;
typedef int (get_block_t)(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);

/* ---- printk / WARN / 比較 */
#define printk(...) usbmem_printk(__VA_ARGS__)
#define WARN(cond, ...) WARN_ON(cond)
#define in_range(val, start, len) ((val) >= (start) && (val) - (start) < (len))
static inline void *memscan(void *addr, int c, size_t size) { unsigned char *p = addr; while (size) { if (*p == (unsigned char)c) return p; p++; size--; } return p; }
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
static inline u32 get_random_u32_below(u32 n) { return n ? get_random_u32() % n : 0; }
#define ktime_get_real_seconds() ((s64)time(NULL))

/* ---- little-endian ビットマップ (ext2: ディスク上のビットマップはバイト列。LE ホストでは通常のビット操作と同じ並び) */
#ifndef __test_and_set_bit_le
#define __test_and_set_bit_le(nr, a) test_and_set_bit((nr), (volatile unsigned long *)(a))
#define __test_and_clear_bit_le(nr, a) test_and_clear_bit((nr), (volatile unsigned long *)(a))
#endif
#ifndef test_bit_le
#define test_bit_le(nr, a) ((((const unsigned char *)(a))[(nr) >> 3] >> ((nr) & 7)) & 1)
#endif
#ifndef find_first_zero_bit_le
#define find_first_zero_bit_le(a, size) find_next_zero_bit((const unsigned long *)(a), (size), 0)
#endif

/* ---- ロック: spin_trylock / rwlock / guard (cleanup 属性で書いたスコープ付きロック) */
#ifndef spin_trylock
#define spin_trylock(x) (pthread_mutex_trylock(&(x)->m) == 0)
#endif
#define read_lock(x) pthread_rwlock_rdlock(&(x)->l)
#define read_unlock(x) pthread_rwlock_unlock(&(x)->l)
#define write_lock(x) pthread_rwlock_wrlock(&(x)->l)
#define write_unlock(x) pthread_rwlock_unlock(&(x)->l)
struct usbmem_guard { spinlock_t *l; };
static inline struct usbmem_guard usbmem_guard_take(spinlock_t *l) { spin_lock(l); struct usbmem_guard g = { l }; return g; }
static inline void usbmem_guard_release(struct usbmem_guard *g) { if (g->l) spin_unlock(g->l); }
#define USBMEM_CAT2(a, b) a##b
#define USBMEM_CAT(a, b) USBMEM_CAT2(a, b)
#define guard(kind) usbmem_guard_##kind
#define usbmem_guard_spinlock(lk) struct usbmem_guard USBMEM_CAT(_ug, __LINE__) __attribute__((cleanup(usbmem_guard_release), unused)) = usbmem_guard_take(lk)
#define usbmem_guard_spinlock_init(lk) struct usbmem_guard USBMEM_CAT(_ug, __LINE__) __attribute__((cleanup(usbmem_guard_release), unused)) = (spin_lock_init(lk), usbmem_guard_take(lk))
#define scoped_guard(kind, lk) for (struct usbmem_guard _sg __attribute__((cleanup(usbmem_guard_release))) = usbmem_guard_take(lk), *_so = &_sg; _so; _so = NULL)

/* ---- uidgid / 時刻 / デバイス番号 */
#define init_user_ns_ ((struct user_namespace *)0)
extern struct user_namespace init_user_ns;
extern struct mnt_idmap nop_mnt_idmap;
static inline bool i_uid_needs_update(struct mnt_idmap *m, const struct iattr *a, const struct inode *i) { (void)m; (void)a; (void)i; return false; }
static inline bool i_gid_needs_update(struct mnt_idmap *m, const struct iattr *a, const struct inode *i) { (void)m; (void)a; (void)i; return false; }
static inline bool in_group_p(kgid_t g) { (void)g; return false; }
static inline time64_t inode_get_atime_sec(const struct inode *i) { return i->i_atime_.tv_sec; }
static inline time64_t inode_get_mtime_sec(const struct inode *i) { return i->i_mtime_.tv_sec; }
static inline time64_t inode_get_ctime_sec(const struct inode *i) { return i->i_ctime_.tv_sec; }
static inline void inode_set_atime(struct inode *i, time64_t s, long ns) { i->i_atime_.tv_sec = s; i->i_atime_.tv_nsec = ns; }
static inline void inode_set_mtime(struct inode *i, time64_t s, long ns) { i->i_mtime_.tv_sec = s; i->i_mtime_.tv_nsec = ns; }
static inline void inode_set_ctime(struct inode *i, time64_t s, long ns) { i->i_ctime_.tv_sec = s; i->i_ctime_.tv_nsec = ns; }
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)
static inline u32 new_encode_dev(dev_t dev) { unsigned major = (dev >> 8) & 0xfff, minor = (dev & 0xff) | ((dev >> 12) & 0xfff00); return (minor & 0xff) | (major << 8) | ((minor & ~0xffu) << 12); }
static inline dev_t new_decode_dev(u32 dev) { unsigned major = (dev & 0xfff00) >> 8, minor = (dev & 0xff) | ((dev >> 12) & 0xfff00); return ((dev_t)major << 8) | (minor & 0xff) | ((dev_t)(minor & ~0xffu) << 12); }
static inline u16 old_encode_dev(dev_t dev) { return (u16)((((dev >> 8) & 0xfff) << 8) | (dev & 0xff)); }
static inline dev_t old_decode_dev(u16 val) { return ((dev_t)((val >> 8) & 255) << 8) | (val & 255); }
static inline int old_valid_dev(dev_t dev) { return ((dev >> 8) & 0xfff) < 256 && (dev & 0xff) < 256; }
static inline __kernel_fsid_t uuid_to_fsid(__u8 *uuid) { __kernel_fsid_t f; memcpy(&f, uuid, sizeof f < 16 ? sizeof f : 16); return f; }
#define NR_FTYPE_ 8
static inline unsigned char fs_umode_to_ftype(umode_t mode) {
  switch (mode & 0170000) { case 0100000: return 1; case 0040000: return 2; case 0020000: return 3; case 0060000: return 4; case 0010000: return 5; case 0140000: return 6; case 0120000: return 7; default: return 0; }
}
static inline unsigned char fs_ftype_to_dtype(unsigned ft) {
  static const unsigned char t[8] = { 0 /*UNKNOWN*/, 8 /*REG*/, 4 /*DIR*/, 2 /*CHR*/, 6 /*BLK*/, 1 /*FIFO*/, 12 /*SOCK*/, 10 /*LNK*/ };
  return ft < 8 ? t[ft] : 0;
}
static inline unsigned long dir_pages(struct inode *i) { return (unsigned long)((i->i_size + PAGE_SIZE - 1) >> PAGE_SHIFT); }

/* ---- inode のライフサイクル / bad inode */
struct inode *iget_locked(struct super_block *, unsigned long ino);
void iget_failed(struct inode *);
void make_bad_inode(struct inode *);
bool is_bad_inode(struct inode *);
static inline void inode_inc_link_count(struct inode *i) { i->i_nlink++; mark_inode_dirty(i); }
static inline void inode_dec_link_count(struct inode *i) { i->i_nlink--; mark_inode_dirty(i); }
static inline void inode_nohighmem(struct inode *i) { (void)i; }
static inline int insert_inode_locked(struct inode *i) { (void)i; usbmem_unimplemented("insert_inode_locked"); return -ENOSYS; }
static inline void set_inode_metadata_writeback(struct inode *i) { (void)i; }
static inline int sync_inode_metadata(struct inode *i, int wait) { (void)i; (void)wait; return 0; }
static inline int d_tmpfile(struct file *f, struct inode *i) { (void)f; (void)i; return -ENOSYS; }
static inline int finish_open_simple(struct file *f, int err) { (void)f; return err; }
#define IS_NOQUOTA(i) 0
#define is_quota_modification(m, i, a) 0
static inline int iov_iter_alignment(const struct iov_iter *i) { (void)i; return 0; }
extern const struct qstr dotdot_name;
extern const struct qstr slash_name;
static inline sector_t sb_bdev_nr_blocks(struct super_block *sb) { return usbmem_bdev_size(sb->s_bdev->impl) >> sb->s_blocksize_bits; }
static inline struct kmem_cache *kmem_cache_create_usercopy(const char *name, unsigned size, unsigned align, unsigned long flags, unsigned useroffset, unsigned usersize, void (*ctor)(void *)) {
  (void)useroffset; (void)usersize; return kmem_cache_create(name, size, align, flags, ctor);
}

/* ---- folio の "checked" ビット (dir.c の検査済みページ印) */
#define FOLIO_CHECKED_BIT 10
static inline void folio_set_checked(struct folio *f) { f->flags |= 1UL << FOLIO_CHECKED_BIT; }
static inline int folio_test_checked(const struct folio *f) { return (f->flags >> FOLIO_CHECKED_BIT) & 1; }
static inline void folio_release_kmap(struct folio *f, const void *addr) { (void)addr; folio_put(f); }

/* ---- ページキャッシュ経由のファイル I/O: mpage (src/rt_buffer.c)。ファイルデータの読み取りは read_folio → mpage_read_folio → get_block */
int mpage_read_folio(struct folio *, get_block_t *);
void mpage_readahead(struct readahead_control *, get_block_t *);
int block_dirty_folio(struct address_space *, struct folio *);
void block_invalidate_folio(struct folio *, size_t, size_t);
bool block_is_partially_uptodate(struct folio *, size_t, size_t);
int buffer_migrate_folio(struct address_space *, struct folio *, struct folio *, int mode);
int generic_block_bmap(struct address_space *, sector_t, get_block_t *);
int simple_fsync(struct file *, loff_t, loff_t, int);
int generic_file_mmap_prepare(void *desc);
unsigned long thp_get_unmapped_area(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
const char *page_get_link(struct dentry *, struct inode *, struct delayed_call *);
const char *simple_get_link(struct dentry *, struct inode *, struct delayed_call *);
int page_symlink(struct inode *, const char *, int);
loff_t generic_llseek_cookie(struct file *, loff_t, int, u64 *);
ssize_t generic_file_write_iter(struct kiocb *, struct iov_iter *);
ssize_t generic_perform_write(struct kiocb *, struct iov_iter *);
int generic_write_end(const struct kiocb *, struct address_space *, loff_t, unsigned, unsigned, struct folio *, void *);
int block_write_begin(struct address_space *, loff_t, unsigned, struct folio **, get_block_t *);
int __block_write_begin(struct folio *, loff_t, unsigned, get_block_t *);
int block_write_end(loff_t, unsigned, unsigned, struct folio *);
int block_truncate_page(struct address_space *, loff_t, get_block_t *);
int mpage_writepages(struct address_space *, struct writeback_control *, get_block_t *);
#define nd_terminate_link(name, len, maxlen) do { if ((len) >= (maxlen)) ((char *)(name))[(maxlen) - 1] = 0; } while (0)

#ifdef __cplusplus
}
#endif
#endif
