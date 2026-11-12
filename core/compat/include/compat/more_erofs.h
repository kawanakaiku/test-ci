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
#endif

#ifdef __cplusplus
}
#endif
#endif
