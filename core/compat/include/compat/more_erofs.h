/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * erofs (fs/erofs) のための追加分。USBMEM_FS_EROFS を定義した翻訳単位だけが読む (more_zfs.h から条件付きで include)。
 * 構成: 読み取り専用・ブロックデバイス (ホストの BlockDevice) のみ。xattr / ACL / fscache / ファイルバック / page cache share / DAX / sysfs は無効。
 * 本体: src/rt_erofs.c。docs/erofs-port.md
 */
#ifndef USBMEM_COMPAT_MORE_EROFS_H
#define USBMEM_COMPAT_MORE_EROFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ビット/算術 */
#ifndef GENMASK
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#endif
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#endif
#ifndef BIT_ULL
#define BIT_ULL(n) (1ULL << (n))
#endif
#ifndef ilog2
#define ilog2(n) ((unsigned)(63 - __builtin_clzll((unsigned long long)(n))))
#endif
#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif
#ifndef roundup_pow_of_two
static inline unsigned long roundup_pow_of_two(unsigned long n) { return n <= 1 ? 1 : 1UL << (64 - __builtin_clzl(n - 1)); }
#endif
#ifndef DIV_ROUND_UP_POW2
#define DIV_ROUND_UP_POW2(n, d) (((n) + (d) - 1) >> ilog2(d))
#endif
#ifndef in_range32
#define in_range32(val, start, len) ((u32)((val) - (start)) < (u32)(len))
#endif
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & ~((unsigned long)PAGE_SIZE - 1))
#endif
#ifndef for_each_set_bit
#define for_each_set_bit(bit, addr, size) \
  for ((bit) = 0; (bit) < (size); (bit)++) if (!(((const unsigned long *)(addr))[(bit) / 64] >> ((bit) % 64) & 1)) { } else
#endif
#ifndef __read_mostly
#define __read_mostly
#endif
#ifndef __GFP_RECLAIM
#define __GFP_RECLAIM 0
#endif
#ifndef DO_ONCE_LITE_IF
#define DO_ONCE_LITE_IF(cond, func, ...) do { if (cond) func(__VA_ARGS__); } while (0)
#endif
#ifndef __acquire
#define __acquire(x) ((void)0)
#define __release(x) ((void)0)
#endif
#ifndef kfree_sensitive
#define kfree_sensitive(p) kfree(p)
#endif
char *kstrndup(const char *s, size_t max, gfp_t gfp);
#ifndef no_free_ptr
#define no_free_ptr(p) ({ __typeof__(p) _nf = (p); (p) = NULL; _nf; })
#endif

/* ---- CPU / スケジューラ (単一スレッド) */
#define num_possible_cpus() 1U
#define raw_smp_processor_id() 0
#define migrate_disable() ((void)0)
#define migrate_enable() ((void)0)
#define DEFINE_SPINLOCK(x) spinlock_t x = { PTHREAD_MUTEX_INITIALIZER }

/* ---- メモリ (page = folio をラップしたホスト側 4096 バイト) */
static inline void set_page_private(struct page *p, unsigned long v) { p->private = v; }
static inline unsigned long page_private(const struct page *p) { return p->private; }
static inline void *page_address(const struct page *p) { return p->folio_->data; }
#define PageHighMem(p) 0
static inline void copy_highpage(struct page *to, struct page *from) { memcpy(to->folio_->data, from->folio_->data, PAGE_SIZE); }
int page_ref_count(const struct page *p);
void __free_page(struct page *p);
unsigned long alloc_pages_bulk(gfp_t gfp, unsigned long nr, struct page **pages);
void *vm_map_ram(struct page **pages, unsigned int count, int node);
void vm_unmap_ram(const void *mem, unsigned int count);
#define vm_unmap_aliases() ((void)0)
static inline unsigned long clear_user(void *to, unsigned long n) { memset(to, 0, n); return 0; }
#define mapping_set_large_folios(m) ((void)0)
#define readahead_gfp_mask(m) 0
static inline struct inode *folio_inode(struct folio *f) { return f->mapping->host; }
static inline struct page *folio_file_page(struct folio *f, pgoff_t idx) { (void)idx; return &f->page; }   /* folio は 1 ページ */
static inline void folio_attach_private(struct folio *f, void *data) { f->private = data; }
static inline void *folio_detach_private(struct folio *f) { void *d = f->private; f->private = NULL; return d; }
static inline struct folio *read_cache_folio(struct address_space *m, pgoff_t idx, void *filler, struct file *file) {
  (void)filler; return read_mapping_folio(m, idx, file);
}
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *to, ssize_t already);
#define noop_direct_IO NULL
#define dax_iomap_rw(...) (-EOPNOTSUPP)

static inline bool dir_emit_dot(struct file *f, struct dir_context *ctx) { return dir_emit(ctx, ".", 1, f->f_inode ? f->f_inode->i_ino : 0, DT_DIR); }

/* ---- idr (erofs: 追加デバイスの管理。ホストでは追加デバイスを拒否するので常にほぼ空) */
struct idr { void **slots_; unsigned n_; };
void idr_init(struct idr *);
void idr_destroy(struct idr *);
int idr_alloc(struct idr *, void *ptr, int start, int end, gfp_t gfp);
void *idr_find(const struct idr *, unsigned long id);
int idr_for_each(const struct idr *, int (*fn)(int id, void *p, void *data), void *data);
#define idr_for_each_entry(idr, entry, id) \
  for ((id) = 0; (id) < (int)(idr)->n_; (id)++) if (!((entry) = (__typeof__(entry))(idr)->slots_[(id)])) { } else

/* ---- マウント (fs_context) まわり */
#define invalf(fc, ...) ((void)(fc), -EINVAL)
#define invalfc(fc, ...) ((void)(fc), -EINVAL)
#define errorfc(fc, ...) ((void)(fc))
#define fsparam_file_or_string(n, o) fsparam_string(n, o)
#define GET_TREE_BDEV_QUIET_LOOKUP 1
static inline int get_tree_bdev_flags(struct fs_context *fc, int (*fill)(struct super_block *, struct fs_context *), unsigned flags) { (void)flags; return get_tree_bdev(fc, fill); }
static inline int get_tree_nodev(struct fs_context *fc, int (*fill)(struct super_block *, struct fs_context *)) { (void)fc; (void)fill; return -EINVAL; }
static inline int get_tree_super(struct fs_context *fc, int (*test)(struct super_block *, struct fs_context *), int (*fill)(struct super_block *, struct fs_context *)) { (void)fc; (void)test; (void)fill; return -EINVAL; }
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#define BLK_OPEN_READ 1
#define filp_open(name, flags, mode) ((struct file *)ERR_PTR(-ENOENT))
#define fs_bdev_file_open_by_path(path, mode, holder, ops) ((struct file *)ERR_PTR(-EOPNOTSUPP))
#define fput(f) ((void)(f))
#define super_set_sysfs_name_id(sb) ((void)(sb))
#define super_setup_bdi(sb) 0
#define MAJOR(dev) ((unsigned)(dev) >> 20)
#define MINOR(dev) ((unsigned)(dev) & 0xfffff)
#ifndef SB_NOATIME
#define SB_NOATIME (1 << 10)
#endif
#ifndef SB_I_RETIRED
#define SB_I_RETIRED 0x00000800
#endif
static inline void kill_anon_super(struct super_block *sb) { (void)sb; }
static inline bool uuid_is_null(const void *u) { const unsigned char *p = (const unsigned char *)u; for (int i = 0; i < 16; i++) if (p[i]) return false; return true; }

/* ---- printk の補助 (_erofs_printk がレベル接頭辞を解釈する) */
#ifndef KERN_SOH_ASCII
#define KERN_SOH_ASCII '\001'
#endif
static inline int printk_get_level(const char *buf) {
  if (buf[0] == KERN_SOH_ASCII && buf[1]) { switch (buf[1]) { case '0' ... '7': case 'c': return buf[1]; } }
  return 0;
}
static inline const char *printk_skip_level(const char *buf) { return printk_get_level(buf) ? buf + 2 : buf; }

/* ---- 展開側 (EROFS_FS_ZIP) の補助 */
#ifdef CONFIG_EROFS_FS_ZIP
#define SHRINK_EMPTY (~0UL - 1)
int xa_empty(const struct xarray *xa);
/* IS_ENABLED(): 共有ヘッダ (more_ext4.h) では常に 0。erofs は CONFIG_EROFS_FS_ZIP を -D で 1 にするので、カーネルと同じ __is_defined の方式にする */
#undef IS_ENABLED
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define __is_defined(x) ___is_defined(x)
#define IS_ENABLED(option) __is_defined(option)
/* 大域バッファ (zutil.c) の vmap/vunmap: NTFS の既存の挙動 (vmap = 未実装スタブ) を変えないよう、erofs の翻訳単位だけマクロで差し替える。
 * 実体は連続した通常のメモリ (ページ配列とは独立。大域バッファはこの連続領域だけを使い、ページ側は触らない)。 */
void *usbmem_erofs_vmap(struct page **pages, unsigned int count);
void usbmem_erofs_vunmap(const void *mem);
#define vmap(pages, count, flags, prot) usbmem_erofs_vmap((pages), (count))
#define vunmap(mem) usbmem_erofs_vunmap(mem)
#define module_param_named(name, var, type, perm)
#define module_param(name, type, perm)
/* mutex の guard() */
struct usbmem_mguard { struct mutex *m; };
static inline struct usbmem_mguard usbmem_mguard_take(struct mutex *m) { mutex_lock(m); struct usbmem_mguard g = { m }; return g; }
static inline void usbmem_mguard_release(struct usbmem_mguard *g) { if (g->m) mutex_unlock(g->m); }
#define usbmem_guard_mutex(lk) struct usbmem_mguard USBMEM_CAT(_ugm, __LINE__) __attribute__((cleanup(usbmem_mguard_release), unused)) = usbmem_mguard_take(lk)
/* ビット待ち: 単一スレッドなので、立っていなければ立てて 0 を返す */
#define wait_on_bit_lock(addr, bit, mode) (test_and_set_bit((bit), (addr)) ? 0 : 0)
#define clear_and_wake_up_bit(bit, addr) clear_bit((bit), (addr))
#define __GFP_NORETRY 0
#define __GFP_DIRECT_RECLAIM 0
#define __GFP_NOMEMALLOC 0
#define POISON_POINTER_DELTA 0
#define atomic_add_return(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
static inline void atomic_long_add(long i, atomic_long_t *a) { a->counter += i; }
#define bio_in_atomic() 0                 /* 展開は常に呼び出し元のスレッドで同期実行 (ワーカーなし) */
#define memalloc_noio_save() 0
#define memalloc_noio_restore(x) ((void)(x))
typedef int blk_status_t;
#define BIO_MAX_VECS 256
#define PageWorkingset(p) 0
#define wait_for_completion_io(c) wait_for_completion(c)
struct kthread_work { int dummy_; };
#define DECLARE_WAIT_QUEUE_HEAD(n) wait_queue_head_t n = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
#define readahead_folio_last(r) ((struct folio *)NULL)
static inline struct folio *filemap_alloc_folio(gfp_t g, unsigned int order, void *policy) { (void)order; (void)policy; struct page *p = alloc_page(g); return p ? page_folio(p) : NULL; }
static inline int folio_trylock(struct folio *f) { if (folio_test_locked(f)) return 0; folio_lock(f); return 1; }
static inline void *folio_get_private(struct folio *f) { return f->private; }
static inline int folio_test_private(const struct folio *f) { return f->private != NULL; }
/* lockref (spinlock + 参照カウント。単一スレッドなので素朴に) */
struct lockref { spinlock_t lock; int count; };
static inline void lockref_init(struct lockref *l) { spin_lock_init(&l->lock); l->count = 1; }
static inline bool lockref_get_not_zero(struct lockref *l) { if (l->count > 0) { l->count++; return true; } return false; }
static inline bool lockref_is_dead(const struct lockref *l) { return l->count < 0; }
static inline void lockref_mark_dead(struct lockref *l) { l->count = -128; }
static inline bool lockref_put_or_lock(struct lockref *l) { if (l->count > 1) { l->count--; return true; } spin_lock(&l->lock); return false; }
/* xarray の最小部分 (添字 → ポインタ。実装は rt_erofs.c の素朴な表) */
void xa_init(struct xarray *xa);
void *xa_load(struct xarray *xa, unsigned long idx);
void *__xa_cmpxchg(struct xarray *xa, unsigned long idx, void *old, void *entry, gfp_t gfp);
void *__xa_erase(struct xarray *xa, unsigned long idx);
void *xa_find(struct xarray *xa, unsigned long *idx, unsigned long max);
void *xa_find_after(struct xarray *xa, unsigned long *idx, unsigned long max);
#define xa_lock(xa) ((void)(xa))
#define xa_unlock(xa) ((void)(xa))
#define xa_trylock(xa) (1)
#define xa_is_err(p) (0)
#define xa_err(p) (0)
#define xa_for_each(xa, index, entry) \
  for ((index) = 0, (entry) = xa_find((xa), &(index), ~0UL); (entry); (entry) = xa_find_after((xa), &(index), ~0UL))
#endif

#ifdef __cplusplus
}
#endif
#endif
