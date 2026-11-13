/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fs/exfat の移植 (FAT 系・buffer_head を使う FS 向け) で追加した互換 API の宣言。
 * 書き込み・ioctl・mount オプション系は、ホスト入口から呼ばれないものがほとんど (呼ばれたら ENOSYS / 無害な値を返す)。
 * 実装: src/rt_bh.c (buffer_head 以外の補助も同居)。
 */
#ifndef USBMEM_COMPAT_MORE_H
#define USBMEM_COMPAT_MORE_H

#ifdef __cplusplus
extern "C" {
#endif

struct buffer_head;

/* ---- atomic 64 */
static inline bool atomic64_try_cmpxchg(atomic64_t *a, s64 *old, s64 nw) { long long o = *old; bool ok = __atomic_compare_exchange_n(&a->counter, &o, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *old = o; return ok; }

/* ---- hlist / rcu / list 補助 */
struct hlist_node { struct hlist_node *next, **pprev; };
struct hlist_head { struct hlist_node *first; };
struct rcu_head { struct rcu_head *next; void (*func)(struct rcu_head *); };
#define INIT_HLIST_HEAD(h) ((h)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *n) { n->next = NULL; n->pprev = NULL; }
static inline int hlist_unhashed(const struct hlist_node *n) { return !n->pprev; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h) {
  n->next = h->first; if (h->first) h->first->pprev = &n->next; h->first = n; n->pprev = &h->first;
}
static inline void hlist_del_init(struct hlist_node *n) {
  if (!n->pprev) return;
  *n->pprev = n->next; if (n->next) n->next->pprev = n->pprev; n->next = NULL; n->pprev = NULL;
}
#define hlist_entry(p, t, m) container_of(p, t, m)
#define hlist_for_each_entry(pos, head, member) \
  for (pos = (head)->first ? hlist_entry((head)->first, __typeof__(*pos), member) : NULL; pos; \
       pos = pos->member.next ? hlist_entry(pos->member.next, __typeof__(*pos), member) : NULL)
static inline void call_rcu(struct rcu_head *h, void (*fn)(struct rcu_head *)) { fn(h); }   /* 単一スレッド前提: 猶予期間なしで即実行 */
static inline u32 hash_32(u32 val, unsigned bits) { return bits >= 32 ? val * 0x61C88647u : (val * 0x61C88647u) >> (32 - bits); }
static inline void list_del_init(struct list_head *e) { list_del(e); INIT_LIST_HEAD(e); }
static inline void list_add(struct list_head *n, struct list_head *h) { n->next = h->next; n->prev = h; h->next->prev = n; h->next = n; }
static inline void list_move(struct list_head *e, struct list_head *h) { e->prev->next = e->next; e->next->prev = e->prev; list_add(e, h); }
#define list_for_each_entry(pos, head, member) \
  for (pos = list_entry((head)->next, __typeof__(*pos), member); &pos->member != (head); pos = list_entry(pos->member.next, __typeof__(*pos), member))

/* ---- ビット操作 (little-endian 前提 = 通常版と同一ビット並び) */
#define set_bit_le(nr, a) __set_bit(nr, (unsigned long *)(a))
#define clear_bit_le(nr, a) clear_bit(nr, (unsigned long *)(a))
#define test_bit_le(nr, a) test_bit(nr, (const unsigned long *)(a))
static inline unsigned long ffz(unsigned long w) { return (unsigned long)__builtin_ctzl(~w); }
static inline unsigned long hweight_long(unsigned long w) { return (unsigned long)__builtin_popcountl(w); }
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))
void *memchr_inv(const void *s, int c, size_t n);
u32 get_random_u32(void);

/* ---- ratelimit / printk */
struct ratelimit_state { int interval, burst; };
#define DEFAULT_RATELIMIT_INTERVAL (5 * 1000)
#define DEFAULT_RATELIMIT_BURST 10
static inline void ratelimit_state_init(struct ratelimit_state *r, int interval, int burst) { r->interval = interval; r->burst = burst; }
static inline int __ratelimit(struct ratelimit_state *r) { (void)r; return 1; }
#define pr_warn(...) usbmem_printk(__VA_ARGS__)
#define pr_debug(...) ((void)0)
#define __cold

/* ---- 名前ハッシュ / 文字 */
static inline unsigned long init_name_hash(const void *salt) { (void)salt; return 0; }
static inline unsigned long partial_name_hash(unsigned long c, unsigned long prev) { return (prev + (c << 4) + (c >> 4)) * 11; }
static inline unsigned int end_name_hash(unsigned long h) { return hash_32((u32)h, 32); }
typedef u32 unicode_t;
int utf8_to_utf32(const u8 *s, int len, unicode_t *pu);
#ifndef __ANDROID__
typedef long __kernel_off_t;
#endif

/* ---- 時刻 (exfat の FAT 形式日時 ↔ unix) */
#include <time.h>   /* struct tm はホストのもの (tm_year は int。カーネルは long だが値域は足りる) */
void time64_to_tm(s64 totalsecs, int offset, struct tm *result);
s64 mktime64(unsigned int year, unsigned int mon, unsigned int day, unsigned int hour, unsigned int min, unsigned int sec);
#define NSEC_PER_MSEC 1000000L
struct usbmem_timezone { int tz_minuteswest; int tz_dsttime; };
extern struct usbmem_timezone sys_tz;   /* 上流は sys_tz.tz_minuteswest だけ使う。ホストは UTC 扱い (0) */
#define MSEC_PER_SEC 1000L

/* ---- uid/gid (vfsuid = idmap 適用後の uid。互換層は idmap なし = 恒等) */
#define from_vfsuid(idmap, ns, v) ((kuid_t){ (v).val })
#define from_vfsgid(idmap, ns, v) ((kgid_t){ (v).val })
#define i_uid_into_vfsuid(idmap, i) ((vfsuid_t){ (i)->i_uid.val })
#define i_gid_into_vfsgid(idmap, i) ((vfsgid_t){ (i)->i_gid.val })
#define vfsuid_eq_kuid(v, k) ((v).val == (k).val)
#define vfsgid_in_group_p(v) 0
#define uid_eq(a, b) ((a).val == (b).val)
#define gid_eq(a, b) ((a).val == (b).val)
#define current_fsuid() ((kuid_t){ 0 })
#define current_uid() ((kuid_t){ 0 })
#define current_gid() ((kgid_t){ 0 })
#define i_user_ns(i) ((struct user_namespace *)0)
#define CAP_LINUX_IMMUTABLE 9
struct fs_struct { int umask; };

/* ---- inode / dentry 補助 */
#define IS_DIRSYNC(i) 0
#define IS_SYNC(i) 0
#define inode_needs_sync(i) 0
#define d_really_is_positive(d) ((d)->d_inode != NULL)
#define inode_peek_iversion_raw(i) ((i)->i_version)
#define inode_query_iversion(i) ((i)->i_version)
#define inode_eq_iversion(i, v) ((i)->i_version == (u64)(v))
unsigned int iunique(struct super_block *, unsigned int max_reserved);
#define __getname() ((char *)malloc(4096))
#define __putname(p) free(p)
#define PTR_ERR_OR_ZERO(p) (IS_ERR(p) ? (int)PTR_ERR(p) : 0)
#define huge_encode_dev(d) ((u64)(d))
#define SB_SYNCHRONOUS (1 << 4)
#define SB_NODIRATIME (1 << 11)
#define ATTR_ATIME_SET (1 << 7)
#define ATTR_MTIME_SET (1 << 8)
#define ATTR_TIMES_SET (1 << 16)
#define FS_XFLAG_CASEFOLD 0x80000000u
#define VM_FAULT_RETRY 0x400
#define LOOKUP_CREATE 0x0200
#define LOOKUP_RENAME_TARGET 0x0800
#define RENAME_NOREPLACE (1 << 0)
#define UTF16_HOST_ENDIAN 0
#define REQ_FUA (1u << 17)
#define REQ_PREFLUSH (1u << 18)
#define IOMAP_F_ZERO_TAIL (1u << 8)
#define IOMAP_F_SIZE_CHANGED (1u << 8)
#define IOMAP_FAULT (1 << 3)
struct backing_dev_info { unsigned long ra_pages, io_pages; };
#define put_user(x, p) ({ *(p) = (x); 0; })

/* ---- fs_parser (mount オプションの定義だけ。解析は fs_parse() が -ENOPARAM を返す = 常に既定値) */
#define fs_param_deprecated 0
#define fs_param_is_u32 0
#define fsparam_uid(n, o) { n, o }
#define fsparam_gid(n, o) { n, o }
#define fsparam_flag_no(n, o) { n, o }
#define __fsparam(t, n, o, f, d) { n, o }
#define WRAP_DIR_ITER(func) static int shared_##func(struct file *file, struct dir_context *ctx) { return func(file, ctx); }

/* ---- ブロック層 (書き込み・discard 系は使わない) */
struct blk_plug { int unused; };
static inline void blk_start_plug(struct blk_plug *p) { (void)p; }
static inline void blk_finish_plug(struct blk_plug *p) { (void)p; }
static inline int sb_issue_discard(struct super_block *sb, sector_t b, sector_t n, gfp_t g, unsigned long f) { (void)sb; (void)b; (void)n; (void)g; (void)f; return -EOPNOTSUPP; }
static inline int sync_blockdev_range(struct block_device *b, loff_t s, loff_t e) { (void)b; (void)s; (void)e; return 0; }
static inline int folio_test_dirty(const struct folio *f) { return (f->flags >> FOLIO_DIRTY) & 1; }
static inline loff_t readahead_pos(const struct readahead_control *r) { (void)r; return 0; }
static inline size_t readahead_length(const struct readahead_control *r) { (void)r; return 0; }
int iomap_bmap(struct address_space *, sector_t, const void *ops);
int __sync_dirty_buffer(struct buffer_head *bh, blk_opf_t op);
#ifndef USBMEM_KBH   /* USBMEM_KBH (ext4/jbd2) は linux/buffer_head.h の実装を使う */
static inline void write_dirty_buffer(struct buffer_head *bh, blk_opf_t op) { (void)bh; (void)op; }
static inline void wait_on_buffer(struct buffer_head *bh) { (void)bh; }
#endif
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 div) { return (u64)(((unsigned __int128)a * mul) / div); }
static inline void balance_dirty_pages_ratelimited(struct address_space *m) { (void)m; }
static inline bool folio_mkclean(struct folio *f) { (void)f; return false; }
static inline int security_inode_setattr(struct mnt_idmap *m, struct dentry *d, struct iattr *a) { (void)m; (void)d; (void)a; return 0; }
static inline int fsnotify_change(struct dentry *d, unsigned int mask) { (void)d; (void)mask; return 0; }
static inline int simple_fsync_noflush(struct file *f, loff_t s, loff_t e, int ds) { (void)f; (void)s; (void)e; (void)ds; return 0; }
static inline vm_fault_t vmf_fs_error(int err) { (void)err; return VM_FAULT_SIGBUS; }
static inline int d_move_(struct dentry *a, struct dentry *b) { (void)a; (void)b; return 0; }
#define d_move(a, b) d_move_(a, b)


/* ---- fs/ext2 移植で追加 (docs/ext2-port.md) */
#ifndef __guarded_by
#define __guarded_by(x)
#define __must_hold(x)
#define __acquires(x)
#define __releases(x)
#define __must_check
#endif

#ifdef __cplusplus
}
#endif
#endif
