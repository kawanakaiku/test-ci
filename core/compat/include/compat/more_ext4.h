/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fs/ext4 (読み取り専用) の移植のために足した API (docs/ext4-port.md)。
 * 読み取りで使うものは実装し、書き込み・ジャーナル・mballoc の割り当てなどは何もしない/-ENOSYS (src/rt_ext4.c)。
 */
#ifndef USBMEM_COMPAT_MORE_EXT4_H
#define USBMEM_COMPAT_MORE_EXT4_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 型・属性 */
#define __nonstring
#define ____cacheline_aligned_in_smp
#define ____cacheline_aligned
#define __percpu
#define __counted_by(x)
#define __rcu
typedef struct { atomic_t refs; } refcount_t;
#define REFCOUNT_INIT(n) { ATOMIC_INIT(n) }
static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned int)atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r) { atomic_inc(&r->refs); }
static inline bool refcount_dec_and_test(refcount_t *r) { return atomic_dec_and_test(&r->refs); }
typedef struct { u32 val; } kprojid_t;
#define KPROJIDT_INIT(v) ((kprojid_t){ v })
#define INVALID_PROJID ((kprojid_t){ (u32)-1 })
static inline kprojid_t make_kprojid(struct user_namespace *ns, u32 projid) { (void)ns; kprojid_t k = { projid }; return k; }
static inline u32 from_kprojid(struct user_namespace *ns, kprojid_t k) { (void)ns; return k.val; }
static inline bool projid_eq(kprojid_t a, kprojid_t b) { return a.val == b.val; }
struct llist_node { struct llist_node *next; };
struct kobject { int dummy_; };
struct kobj_type { int dummy_; };
struct hlist_node; 
struct name_snapshot { struct qstr name; };
typedef s64 ktime_t;
typedef unsigned long long llong_ext4_t;

struct llist_head { struct llist_node *first; };
struct timer_list { int dummy_; };
struct delayed_work { struct work_struct work; };
typedef long long qsize_t;
struct percpu_rw_semaphore { struct rw_semaphore rw_; };
static inline int percpu_init_rwsem(struct percpu_rw_semaphore *s) { init_rwsem(&s->rw_); return 0; }
static inline void percpu_free_rwsem(struct percpu_rw_semaphore *s) { (void)s; }
static inline void percpu_down_read(struct percpu_rw_semaphore *s) { down_read(&s->rw_); }
static inline void percpu_up_read(struct percpu_rw_semaphore *s) { up_read(&s->rw_); }
static inline void percpu_down_write(struct percpu_rw_semaphore *s) { down_write(&s->rw_); }
static inline void percpu_up_write(struct percpu_rw_semaphore *s) { up_write(&s->rw_); }
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
static inline int atomic_add_unless(atomic_t *a, int n, int u) { int c = atomic_read(a); while (c != u) { if (atomic_cmpxchg(a, c, c + n) == c) return 1; c = atomic_read(a); } return 0; }
#define inode_is_locked(i) (1)
#define rwsem_is_locked(s) (1)
void flush_work(struct work_struct *);
u32 crc32c(u32 crc, const void *p, size_t len);   /* カーネルの crc32c(): 前後の反転なし (ext4 は seed に ~0 を渡す)。実装: src/rt_ext4.c */

/* ---- 時間・タイマ・遅延実行 (読み取り専用では動かさない。コンパイルを通すための定義と、何もしない実装) */
#define HZ 1000
extern unsigned long usbmem_jiffies(void);
#define jiffies usbmem_jiffies()
#define msecs_to_jiffies(m) ((unsigned long)(m))
#define secs_to_jiffies(s) ((unsigned long)(s) * 1000UL)
#define nsecs_to_jiffies(n) ((unsigned long)((n) / 1000000ULL))
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_before32(a, b) ((int)((a) - (b)) < 0)
#define timer_setup(t, fn, flags) do { (void)(t); } while (0)
#define timer_container_of(var, t, member) container_of(t, __typeof__(*(var)), member)
static inline int mod_timer(struct timer_list *t, unsigned long e) { (void)t; (void)e; return 0; }
static inline int timer_delete_sync(struct timer_list *t) { (void)t; return 0; }
static inline int timer_shutdown_sync(struct timer_list *t) { (void)t; return 0; }
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static inline bool schedule_work(struct work_struct *w) { (void)w; return false; }
static inline bool schedule_delayed_work(struct delayed_work *w, unsigned long d) { (void)w; (void)d; return false; }
#define schedule_timeout_interruptible(t) (0)
#define might_sleep() do { } while (0)
#define try_to_freeze() (0)
#define set_freezable() do { } while (0)
#define _RET_IP_ ((unsigned long)__builtin_return_address(0))
#define KERN_CONT ""
#define printk_once(...) printk(__VA_ARGS__)
#define pr_warn_once(...) printk(__VA_ARGS__)
#define DUMP_PREFIX_ADDRESS 1
#define DUMP_PREFIX_OFFSET 2
#define DUMP_PREFIX_NONE 0
static inline void print_hex_dump(const char *l, const char *p, int pt, int rs, int gs, const void *b, size_t n, bool a) { (void)l; (void)p; (void)pt; (void)rs; (void)gs; (void)b; (void)n; (void)a; }
#define prefetchw(x) do { } while (0)
#define prefetch(x) do { } while (0)
#define umin(a, b) ((a) < (b) ? (a) : (b))
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((u32)((n) & 0xffffffffu))
static inline u32 rol32(u32 w, unsigned s) { return (w << (s & 31)) | (w >> ((32 - s) & 31)); }
static inline unsigned long memweight(const void *p, size_t n) { const unsigned char *b = p; unsigned long w = 0; for (size_t i = 0; i < n; i++) w += (unsigned long)__builtin_popcount(b[i]); return w; }
#define strtomem_pad(dst, src, pad) do { size_t _n = strnlen((src), sizeof(dst)); memcpy((dst), (src), _n); memset((char *)(dst) + _n, (pad), sizeof(dst) - _n); } while (0)
#define strscpy_pad(...) USBMEM_STRSCPY_PAD_SEL(__VA_ARGS__, 3, 2)(__VA_ARGS__)
#define USBMEM_STRSCPY_PAD_SEL(_1, _2, _3, N, ...) usbmem_strscpy_pad##N
#define usbmem_strscpy_pad3(d, s, n) ({ ssize_t _r = strscpy((d), (s), (n)); if (_r >= 0) memset((char *)(d) + _r, 0, (n) - _r); _r; })
#define usbmem_strscpy_pad2(d, s) usbmem_strscpy_pad3((d), (s), sizeof(d))
struct rb_node *usbmem_rb_first_postorder(const struct rb_root *root);
struct rb_node *usbmem_rb_next_postorder(const struct rb_node *node);
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
  for ((pos) = ({ struct rb_node *_p = usbmem_rb_first_postorder(root); _p ? rb_entry(_p, __typeof__(*(pos)), field) : NULL; }); \
       (pos) && ({ struct rb_node *_q = usbmem_rb_next_postorder(&(pos)->field); (n) = _q ? rb_entry(_q, __typeof__(*(pos)), field) : NULL; 1; }); \
       (pos) = (n))

/* ---- RCU / プリエンプション (単一スレッド的に扱う。読み取り専用のマウントは並行更新が無い) */
#define rcu_dereference(p) (p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw(p) (p)
#define rcu_access_pointer(p) (p)
#define rcu_assign_pointer(p, v) do { (p) = (v); } while (0)
#define RCU_INIT_POINTER(p, v) do { (p) = (v); } while (0)
#define synchronize_rcu() do { } while (0)

#define kfree_rcu(p, f) kfree(p)

/* ---- mempool: kmem_cache を直接使うだけ (プール保証は不要) */
typedef struct mempool { int dummy_; } mempool_t;
#define mempool_create_slab_pool(n, cache) ((mempool_t *)(cache))
#define mempool_alloc(p, gfp) kmem_cache_alloc((struct kmem_cache *)(p), (gfp))
#define mempool_free(e, p) kmem_cache_free((struct kmem_cache *)(p), (e))
#define mempool_destroy(p) do { } while (0)
#define KMEM_CACHE(s, flags) kmem_cache_create5(#s, sizeof(struct s), __alignof__(struct s), (flags), NULL)

/* ---- バッファ・ブロック */
#define BUFFER_TRACE(bh, info) do { } while (0)
#define get_bh(bh) do { (void)(bh); } while (0)
#define put_bh(bh) do { (void)(bh); } while (0)
#define bh2jh(bh) ((struct journal_head *)NULL)
struct io_context { int ioprio; };
#define is_journal_aborted(j) (1)
#define MBE_REUSABLE_B 0
#define set_blocksize(bdev, size) (0)
#define sb_find_get_block(sb, blk) ((struct buffer_head *)NULL)
#define sb_any_quota_suspended(sb) (0)
#define sb_start_intwrite_trylock(sb) (1)
#define super_set_sysfs_name_bdev(sb) do { } while (0)
#define sb_no_casefold_compat_fallback(sb) (0)
#define STAT_WRITE 1
#define part_stat_read(part, field) (0)
#define errseq_check_and_advance(e, s) (0)
#define path_put(p) do { } while (0)
#define PF_MEMALLOC_NOFS 0x00040000
#define IOPRIO_CLASS_BE 2
#define IOPRIO_PRIO_VALUE(c, d) (((c) << 13) | (d))
#define set_task_ioprio(t, p) do { } while (0)
#define IOCB_ATOMIC 0
#define FMODE_32BITHASH 0x200
#define FMODE_64BITHASH 0x400
#define SB_I_VERSION 0
#define __GFP_FS 0
#define __GFP_MOVABLE 0
#define REQ_PRIO 0
#define REQ_RAHEAD 0


/* ---- 書き込み・ライトバック・iomap・ページキャッシュ周辺: 型の定義と、読み取り専用では呼ばれない (呼ばれても無害な) 定義 */
struct xarray { int dummy_; };
typedef unsigned xa_mark_t;
struct folio_batch { unsigned nr; struct folio *folios[31]; };
static inline void folio_batch_init(struct folio_batch *fb) { fb->nr = 0; }
static inline void folio_batch_release(struct folio_batch *fb) { fb->nr = 0; }
struct shrink_control;
struct shrinker { unsigned long (*count_objects)(struct shrinker *, struct shrink_control *); unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *); void *private_data; int seeks; };
struct shrink_control { unsigned long nr_to_scan; unsigned long nr_scanned; gfp_t gfp_mask; };
#define shrinker_alloc(flags, fmt, ...) ((struct shrinker *)kzalloc(sizeof(struct shrinker), GFP_KERNEL))   /* 登録するだけ。メモリ逼迫での回収は起きない */
#define shrinker_register(s) do { } while (0)
#define shrinker_free(s) kfree(s)
#define SHRINK_STOP (~0UL)
struct fiemap_extent_info { unsigned int fi_flags; unsigned int fi_extents_mapped, fi_extents_max; void *fi_extents_start; };
#define FIEMAP_FLAG_CACHE 0x00000004
#define FALLOC_FL_WRITE_ZEROES 0x80
#define PAGECACHE_TAG_DIRTY 0
#define PAGECACHE_TAG_TOWRITE 2
#define FGP_ACCESSED 0x1
#define IOMAP_DAX 0
#define IOMAP_ATOMIC 0
struct seq_operations { void *(*start)(struct seq_file *, loff_t *); void (*stop)(struct seq_file *, void *); void *(*next)(struct seq_file *, void *, loff_t *); int (*show)(struct seq_file *, void *); };
static inline void seq_putc(struct seq_file *m, char c) { (void)m; (void)c; }
typedef u32 projid_t;
#define EXT4_SUPER_MAGIC 0xEF53
#define le32_add_cpu(p, v) do { *(p) = cpu_to_le32(le32_to_cpu(*(p)) + (v)); } while (0)
#define atomic_add(n, v) ((void)__atomic_add_fetch(&(v)->counter, (n), __ATOMIC_SEQ_CST))
#define try_cmpxchg(p, o, n) ({ __typeof__(*(o)) _old = *(o); __typeof__(*(o)) _cur = __sync_val_compare_and_swap((p), _old, (n)); if (_cur != _old) *(o) = _cur; _cur == _old; })
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define task_pid_nr(t) ((t)->pid)
#define dump_stack() do { } while (0)
#define get_order(size) ({ unsigned long _s = ((size) - 1) >> PAGE_SHIFT; int _o = 0; while (_s) { _o++; _s >>= 1; } _o; })
#define get_unaligned_be32(p) be32_to_cpu(*(const u32 *)(p))
#define memalloc_retry_wait(g) do { } while (0)
#define ktime_get() ((ktime_t)0)
#define ktime_get_ns() ((u64)0)
#define ktime_sub(a, b) ((a) - (b))
#define ktime_to_ns(k) ((s64)(k))
#define kthread_run(fn, data, fmt, ...) ((struct task_struct *)ERR_PTR(-ENOSYS))
#define kthread_stop(t) (0)
#define kthread_should_stop() (1)
#define kobject_put(k) do { } while (0)
static inline char *kmemdup_nul(const char *s, size_t n, gfp_t g) { char *p = kmalloc(n + 1, g); if (p) { memcpy(p, s, n); p[n] = 0; } return p; }
#define kmem_cache_zalloc(c, f) kmem_cache_alloc((c), (f) | __GFP_ZERO)
#define kzalloc_flex(v, member, n, ...) kzalloc(sizeof(v) + (size_t)(n) * sizeof(((__typeof__(v) *)0)->member[0]), GFP_KERNEL)
#define init_llist_head(h) do { (h)->first = NULL; } while (0)
static inline bool llist_add(struct llist_node *n, struct llist_head *h) { n->next = h->first; h->first = n; return n->next == NULL; }
static inline struct llist_node *llist_del_all(struct llist_head *h) { struct llist_node *f = h->first; h->first = NULL; return f; }
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_for_each(pos, head) for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)
#define list_for_each_safe(pos, n, head) for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head); (pos) = (n), (n) = (pos)->next)
static inline void list_move_tail(struct list_head *e, struct list_head *h) { list_del(e); list_add_tail(e, h); }
static inline int list_empty_careful(const struct list_head *h) { return h->next == h; }
#define lockdep_assert_held_read(l) do { } while (0)
#define write_trylock(l) (1)
#define down_write_trylock(s) (pthread_rwlock_trywrlock(&(s)->l) == 0)
#define inode_trylock_shared(i) (1)
#define wait_on_bit(w, b, m) (0)
#define test_and_clear_wake_up_bit(b, w) (0)
#define flush_workqueue(w) do { } while (0)
#define flush_delayed_work(w) do { } while (0)
#define percpu_counter_initialized(c) (1)
#define folio_wait_stable(f) do { } while (0)
#define folio_clear_dirty_for_io(f) (0)
#define folio_clear_checked(f) do { } while (0)
#define folio_set_mappedtodisk(f) do { } while (0)
#define folio_maybe_dma_pinned(f) (0)
#define folio_mapped(f) (0)
#define folio_next_index(f) ((f)->index + 1)
#define folio_end_read(f, ok) do { if (ok) folio_mark_uptodate(f); folio_unlock(f); } while (0)
#define folio_zero_range(f, o, l) memset((char *)(f)->data + (o), 0, (l))
#define folio_zero_tail(f, o, kaddr) ((void *)(kaddr))
#define folio_zero_new_buffers(f, from, to) do { } while (0)
#define unmap_mapping_pages(m, s, n, e) do { } while (0)
#define filemap_flush(m) (0)
#define filemap_flush_range(m, s, e) (0)
#define filemap_fdatawrite_range(m, s, e) (0)
#define filemap_dirty_folio(m, f) (0)
#define mapping_tagged(m, t) (0)
#define mapping_gfp_constraint(m, g) (g)
#define mapping_set_folio_order_range(m, a, b) do { } while (0)
#define mapping_min_folio_nrbytes(m) ((size_t)PAGE_SIZE)
#define filemap_get_folio(m, i) ((struct folio *)ERR_PTR(-ENOENT))
#define filemap_get_folios(m, s, e, fb) (0)
#define filemap_get_folios_tag(m, s, e, t, fb) (0)
#define tag_pages_for_writeback(m, s, e) do { } while (0)
#define wbc_to_tag(w) (0)
#define write_begin_get_folio(k, m, i, l) ((struct folio *)ERR_PTR(-ENOSYS))
#define try_to_writeback_inodes_sb(sb, r) do { } while (0)
#define try_to_free_buffers(f) (0)
#define block_commit_write(f, from, to) (0)
#define invalidate_bdev(b) do { } while (0)
#define inode_io_list_del(i) do { } while (0)
#define inode_is_dirtytime_only(i) (0)
#define inode_is_open_for_write(i) (0)
#define iput_if_not_last(i) (0)
#define icount_read_once(i) atomic_read(&(i)->i_count)
#define find_inode_by_ino_rcu(sb, ino) ((struct inode *)NULL)
#define wait_on_new_inode(i) do { } while (0)
#define inode_peek_iversion(i) ((i)->i_version)
#define inode_set_iversion_raw(i, v) ((i)->i_version = (v))
#define inode_set_iversion_queried(i, v) ((i)->i_version = (v))
#define inode_state_read(i) ((i)->i_state)
#define inode_state_clear(i, f) ((i)->i_state &= ~(f))
#define inode_set_cached_link(i, l, n) do { (i)->i_link = (char *)(l); } while (0)
#define inode_fsuid_set(i, idmap) do { } while (0)
#define i_uid_update(idmap, attr, inode) do { } while (0)
#define i_gid_update(idmap, attr, inode) do { } while (0)
#define iov_iter_truncate(i, n) do { if ((i)->count > (n)) (i)->count = (n); } while (0)
#define iomap_write_delalloc_release(...) (0)
#define iomap_last_written_block(i, p, w) (0)
#define iomap_fill_dirty_folios(it, s, e, p) (0)
#define iomap_dio_read_simple(a, b, c) (-ENOSYS)
#define iomap_bio_read_folio(f, o) (-ENOSYS)
#define iomap_bio_readahead(r, o) do { } while (0)
#define generic_set_sb_d_ops(sb) do { } while (0)
#define generic_check_addressable(bits, n) (0)
#define generic_atomic_write_valid(i, it) (false)
#define generic_fill_statx_atomic_writes(...) do { } while (0)
#define generic_ci_validate_strict_name(i, n) (true)
#define fs_dax_get_by_bdev(b, o, p, h) ((void *)NULL)
#define fs_put_dax(d, h) do { } while (0)
#define fs_lookup_param(fc, p, want_bdev, flags, path) (-EINVAL)
#define fs_bdev_file_release(f, sb) do { } while (0)
#define fs_bdev_file_open_by_dev(d, m, h, o) ((struct file *)ERR_PTR(-ENOSYS))
#define fserror_report_metadata(...) do { } while (0)
#define fserror_report_file_metadata(...) do { } while (0)
#define fsparam_string_empty(n, o) fsparam_string(n, o)
#define fsparam_bdev(n, o) fsparam_string(n, o)
#define xattr_prefix(h) ((h)->prefix)
#define xattr_handler_can_list(h, d) (1)
#define vma_desc_set_flags(d, ...) do { } while (0)
#define getblk_unmovable(bdev, blk, size) __getblk((bdev), (blk), (size))
#define sb_getblk_gfp(sb, blk, gfp) __getblk((sb)->s_bdev, (blk), (sb)->s_blocksize)
#define sb_issue_zeroout(sb, b, n, g) (-EIO)
#define bh_uptodate_or_lock(bh) (1)
#define mmb_has_buffers(m) (0)
#define folio_buffers(f) ((struct buffer_head *)NULL)
struct kmem_cache_args { unsigned int align, useroffset, usersize, freeptr_offset; bool use_freeptr_offset; void (*ctor)(void *); };
static inline struct kmem_cache *kmem_cache_create4(const char *name, size_t size, const struct kmem_cache_args *a, unsigned long flags) { return kmem_cache_create5(name, size, a ? a->align : 0, flags, a ? a->ctor : NULL); }
#define USBMEM_KMC_SEL(_1, _2, _3, _4, _5, NAME, ...) NAME
#define kmem_cache_create(...) USBMEM_KMC_SEL(__VA_ARGS__, kmem_cache_create5, kmem_cache_create4, 0)(__VA_ARGS__)
#define system_state 0
#define SYSTEM_HALT 1
#define SYSTEM_POWER_OFF 2
#define SYSTEM_RESTART 3
#define WARN_ONCE(c, ...) WARN_ON(c)
#define clamp_val(v, lo, hi) clamp((v), (lo), (hi))
#define crc16(c, p, n) usbmem_crc16((c), (p), (n))
u16 usbmem_crc16(u16 crc, const void *p, size_t len);
#define file_path(f, buf, n) ((char *)ERR_PTR(-ENOSYS))
#define file_bdev(f) ((struct block_device *)NULL)
#define bdev_read_only(b) (0)
#define bdev_can_atomic_write(b) (false)
#define bdev_atomic_write_unit_min_bytes(b) (0)
#define bdev_atomic_write_unit_max_bytes(b) (0)
#define bdev_dma_alignment(b) (511)
#define bdev_getblk(bdev, blk, size, gfp) __getblk((bdev), (blk), (size))
#define sb_end_write(sb) do { } while (0)
#define sb_start_write_trylock(sb) (1)
void bh_submit(struct buffer_head *bh, blk_opf_t op, bio_end_io_t end_io);
void bh_end_read(struct bio *bio);
void bh_end_write(struct bio *bio);
#define block_read_full_folio(f, g) mpage_read_folio((f), (g))
#define blk_crypto_submit_bio(b) submit_bio(b)
#define readahead_count(r) (0)
#define readahead_index(r) (0)
#define readahead_folio(r) ((struct folio *)NULL)
#define dquot_alloc_space_nodirty(i, n) (0)
#define dquot_free_space_nodirty(i, n) do { } while (0)
#define dquot_initialize_needed(i) (0)
#define d_invalidate(d) do { } while (0)
#define cache_no_acl(i) do { } while (0)
#define __iget(i) ihold(i)
#define block_page_mkwrite(...) (-ENOSYS)
#define INIT_DELAYED_WORK(w, fn) do { (w)->work.func = (fn); } while (0)
#define alloc_workqueue_dummy 0
#define WQ_MEM_RECLAIM 0
#define WQ_UNBOUND 0
#define WQ_HIGHPRI 0
#define NSEC_PER_SEC 1000000000L
#define MAX_PAGECACHE_ORDER 0
#define GFP_NOWAIT 0
#define FS_LBS 0
#define FS_MGTIME 0
#define SB_I_ALLOW_HSM 0
#define SB_INLINECRYPT 0
#define SB_LAZYTIME 0
#define I_DIRTY_TIME 0
#define I_METADATA_WRITEBACK 0
#define PF_MEMALLOC 0x00000800
#define S_VERITY 0
#define S_CASEFOLD 0
#define STATX_WRITE_ATOMIC 0
#define VM_FAULT_LOCKED 0
#define VM_FAULT_NOPAGE 0
#define WHITEOUT_MODE 0
#define WHITEOUT_DEV 0
#define QFMT_VFS_OLD 1
#define QFMT_VFS_V0 2
#define QFMT_VFS_V1 4
#define XATTR_USER_PREFIX "user."
#define XATTR_USER_PREFIX_LEN 5
#define XATTR_TRUSTED_PREFIX "trusted."
#define XATTR_TRUSTED_PREFIX_LEN 8
#define XATTR_HURD_PREFIX "gnu."
#define XATTR_HURD_PREFIX_LEN 4
#define XATTR_SECURITY_PREFIX "security."
#define XATTR_SECURITY_PREFIX_LEN 9
#define set_buffer_mapped(bh) ((bh)->b_state |= 1UL << BH_Mapped)
#define clear_buffer_dirty(bh) ((bh)->b_state &= ~(1UL << BH_Dirty))
#define mb_cache_entry_wait_unused(e) do { } while (0)


/* ---- 追加分 (3) */
#define clamp_t(t, v, lo, hi) ((t)clamp((t)(v), (t)(lo), (t)(hi)))
#define S_ENCRYPTED 0
#define S_DAX 0
#define DEFINE_RATELIMIT_STATE(name, i, b) struct ratelimit_state name = { (i), (b) }
#define DEFAULT_RATELIMIT_INTERVAL (5 * HZ)
#define DEFAULT_RATELIMIT_BURST 10
#define ___ratelimit(rs, func) __ratelimit(rs)
struct seq_file { void *private; };
#define SEQ_START_TOKEN ((void *)1)
#define QSTR_INIT(n, l) { .name = (const unsigned char *)(n), .len = (l) }
#define file_check_and_advance_wb_err(f) (0)
#define dget_parent(d) ((d)->d_parent)
#define d_find_any_alias(i) d_find_alias(i)
#define d_path(p, buf, n) ((char *)ERR_PTR(-ENOSYS))
#define daxdev_mapping_supported(...) (false)
#define IS_NOSEC(i) (1)
#define dax_break_layout_inode(i, f) (0)
#define dax_break_layout_final(i) do { } while (0)
#define dax_zero_range(i, p, l, d, o) (-ENOSYS)
#define dax_writeback_mapping_range(m, d, w) (0)
#define dquot_reserve_block(i, n) (0)
#define dquot_claim_block(i, n) do { } while (0)
#define dquot_release_reservation_block(i, n) do { } while (0)
#define dquot_reclaim_block(i, n) do { } while (0)
#define dquot_alloc_block_nofail(i, n) (0)
#define bio_clear_flag(b, f) do { } while (0)
#define BIO_COMPLETE_IN_TASK 0
#define bio_endio_bh(b, pbh) ({ *(pbh) = NULL; false; })
#define noop_dirty_folio NULL
#define buffer_migrate_folio_norefs NULL
#define iocb_bio_iopoll NULL
#define FOP_DONTCACHE 0
#define FOP_DIO_PARALLEL_WRITE 0
#define FOP_BUFFER_RASYNC 0
#define FOP_MMAP_SYNC 0
#define FMODE_CAN_ATOMIC_WRITE 0
#define IOMAP_DIO_UNWRITTEN 0
#define IOMAP_IOEND_UNWRITTEN 0
#define IOMAP_F_ATOMIC_BIO 0
#define IOMAP_NOWAIT 0
#define WB_SYNC_NONE 0
#define FIEMAP_FLAG_XATTR 0x00000002
#define FALLOC_FL_ZERO_RANGE 0x10
#define bdev_write_zeroes_unmap_sectors(b) (0)
#define fiemap_prep(i, fi, s, l, m) (0)
#define create_empty_buffers(f, bs, st) ((struct buffer_head *)NULL)
#define __folio_start_writeback(f, k) do { } while (0)
#define buffer_jbd(bh) (0)
#define buffer_freed(bh) (0)
#define buffer_req(bh) (0)
#define set_buffer_req(bh) do { } while (0)


/* ---- 追加分 (4) */
#define cmpxchg(p, o, n) __sync_val_compare_and_swap((p), (o), (n))
#define __bforget(bh) bforget(bh)
#define SB_FREEZE_COMPLETE 4
#define dir_relax_shared(i) (1)
#define ra_has_index(ra, idx) (0)
struct vfsmount { struct dentry *mnt_root; struct super_block *mnt_sb; };
#define FIEMAP_EXTENT_LAST 0x00000001
#define FIEMAP_EXTENT_UNKNOWN 0x00000002
#define FIEMAP_EXTENT_DELALLOC 0x00000004
#define FIEMAP_EXTENT_ENCODED 0x00000008
#define FIEMAP_EXTENT_UNWRITTEN 0x00000800
#define FIEMAP_EXTENT_MERGED 0x00001000
#define FIEMAP_EXTENT_DATA_INLINE 0x00000200
#define FIEMAP_EXTENT_NOT_ALIGNED 0x00000100
#define fiemap_fill_next_extent(fi, l, p, len, fl) (-EOPNOTSUPP)

#define init_completion(cp) do { (cp)->done = 0; pthread_mutex_init(&(cp)->m, NULL); pthread_cond_init(&(cp)->c, NULL); } while (0)

/* ext4 だけ call_rcu を「アンマウント後」に遅らせる (put_super が sbi を call_rcu で解放し、その後 kill_sb が sbi を読むため)。他の FS は即時実行 (more.h) のまま */
#ifdef USBMEM_RCU_DEFER
void usbmem_call_rcu_defer(struct rcu_head *h, void (*fn)(struct rcu_head *));
#undef call_rcu
#define call_rcu(h, f) usbmem_call_rcu_defer((h), (f))
#endif

/* ---- 設定 */
#define IS_ENABLED(x) 0
#define IS_BUILTIN(x) 0
#define IS_MODULE(x) 0
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) do { *(volatile __typeof__(x) *)&(x) = (v); } while (0)
#define data_race(x) (x)
#define IS_ENCRYPTED(i) 0
#define IS_CASEFOLDED(i) 0
#define IS_VERITY(i) 0
#define IS_DAX(i) 0
#define IS_SYNC(i) 0
#define IS_DIRSYNC(i) 0
#define IS_NOQUOTA(i) 0

#ifdef __cplusplus
}
#endif
#endif
