/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fs/ext4 + fs/jbd2 (USBMEM_KBH) の翻訳単位だけに足す互換定義 (docs/ext4-write.md)。
 * more_ext4.h の「読み取り専用なので何もしない」定義のうち、書き込み・ジャーナルで本当に動く必要があるものをここで置き換える。
 * 実装: src/rt_kbh.c (buffer_head)、src/rt_jbd2.c (待ち・カーネルスレッドの代わり・タイマ・その他)。
 *
 * 実行モデル: 互換層は単一スレッドで動く。jbd2 のコミットスレッド (kjournald2) は作らず、「待ち」に入った時点
 * (wait_event / schedule) で、呼び出し元のスレッドがコミット要求を処理する (src/rt_jbd2.c の usbmem_jbd2_poll)。
 * 待っても条件が成り立たない = 他に条件を変えられる実行主体がいない = デッドロックなので、黙って止まらずに中断する。
 */
#ifndef USBMEM_COMPAT_MORE_JBD2_H
#define USBMEM_COMPAT_MORE_JBD2_H
#include <linux/percpu_counter.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- 定数 */
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#define BDEVNAME_SIZE 32
#ifndef SZ_16K
#define SZ_16K 0x4000
#endif
#ifndef SZ_1M
#define SZ_1M 0x100000
#endif
#ifndef REQ_IDLE
#define REQ_IDLE 0
#endif
#define BLK_MAX_SEGMENT_SIZE 65536
#define system_dfl_wq ((struct workqueue_struct *)1)

/* ---- 待ち・スケジュール (単一スレッド。待つ代わりに保留の仕事とジャーナルのコミット要求を処理する) */
void usbmem_jbd2_schedule(void);
void usbmem_jbd2_stuck(const char *what, const char *file, int line) __attribute__((noreturn));
#undef schedule
#define schedule() usbmem_jbd2_schedule()
#undef wait_event
#define wait_event(wq, cond) do { if (!(cond)) { usbmem_compat_run_deferred_work(); \
    if (!(cond)) usbmem_jbd2_stuck(#cond, __FILE__, __LINE__); } } while (0)
#define wait_event_interruptible(wq, cond) ({ wait_event(wq, cond); 0; })
#define io_schedule() usbmem_jbd2_schedule()
#define schedule_timeout_uninterruptible(t) (usbmem_jbd2_schedule(), 0)
#define schedule_timeout_interruptible(t) (usbmem_jbd2_schedule(), 0)
#define wait_on_bit(word, bit, mode) ({ if (test_bit((bit), (word))) { usbmem_compat_run_deferred_work(); \
    if (test_bit((bit), (word))) usbmem_jbd2_stuck("wait_on_bit", __FILE__, __LINE__); } 0; })
#define wait_on_bit_io(word, bit, mode) wait_on_bit(word, bit, mode)
#define wake_up_bit(word, bit) do { (void)(word); (void)(bit); } while (0)
#define clear_and_wake_up_bit(bit, word) clear_bit((bit), (word))
#define test_and_clear_wake_up_bit(bit, word) test_and_clear_bit((bit), (unsigned long *)(word))
#define wake_up_process(t) ((void)(t))
#define try_to_freeze() (0)
#define set_freezable() do { } while (0)
#define freezing(t) (0)
#define need_resched() (0)
#define spin_needbreak(l) (0)
#define cond_resched_lock(l) (0)
#define assert_spin_locked(l) do { } while (0)
#define spin_is_locked(l) (1)
#define lockdep_is_held(l) (1)
#define __acquire(x) do { } while (0)
#define __release(x) do { } while (0)
#define mutex_lock_io(m) mutex_lock(m)
#define DEFINE_WAIT(name) wait_queue_entry_t name = { .entry = { &name.entry, &name.entry } }
#define prepare_to_wait(wq, w, st) do { (void)(wq); (void)(w); } while (0)

/* ---- bit spinlock (単一スレッド: 取れなければ再入 = バグ) */
static inline void bit_spin_lock(int bitnum, unsigned long *addr) { if (test_and_set_bit(bitnum, addr)) usbmem_jbd2_stuck("bit_spin_lock", __FILE__, __LINE__); }
static inline void bit_spin_unlock(int bitnum, unsigned long *addr) { clear_bit(bitnum, addr); }
static inline int bit_spin_is_locked(int bitnum, unsigned long *addr) { return test_bit(bitnum, addr); }

/* ---- タイマ (発火しない: コミットは明示的な同期・容量・アンマウントで行う) */
struct timer_list { unsigned long expires; void (*function)(struct timer_list *); unsigned int flags; };
#define timer_setup(t, fn, fl) do { (t)->function = (fn); (t)->expires = 0; (t)->flags = (fl); } while (0)
static inline int mod_timer(struct timer_list *t, unsigned long e) { t->expires = e; return 0; }
static inline void add_timer(struct timer_list *t) { (void)t; }
#define timer_delete(t) (0)   /* glibc の timer_delete (time.h) と名前が重なるのでマクロ */
static inline int timer_delete_sync(struct timer_list *t) { (void)t; return 0; }
static inline int timer_shutdown_sync(struct timer_list *t) { (void)t; return 0; }
#define round_jiffies_up(j) (j)
#define round_jiffies(j) (j)
#define ktime_add_ns(k, n) ((k) + (ktime_t)(n))
#define get_cycles() ((unsigned long)0)

/* ---- カーネルスレッド: jbd2 の kjournald2 だけを「待ちの時に処理する」形で受け付ける (src/rt_jbd2.c) */
struct task_struct *usbmem_kthread_run(int (*fn)(void *), void *data, const char *name, const char *arg);
int usbmem_kthread_stop(struct task_struct *t);
#define kthread_run(fn, data, fmt, ...) usbmem_kthread_run((fn), (data), (fmt), #__VA_ARGS__)
#define kthread_stop(t) usbmem_kthread_stop(t)
#define kthread_should_stop() (1)

/* ---- その他の小物 */
#define noinline_for_stack noinline
#define cpu_to_be64(x) __builtin_bswap64(x)
#define be64_to_cpu(x) __builtin_bswap64(x)
#ifndef pr_emerg
#define pr_emerg(...) usbmem_printk(__VA_ARGS__)
#endif
#ifndef pr_notice
#define pr_notice(...) ((void)0)
#endif
#define SLAB_TEMPORARY 0
extern struct super_block usbmem_blockdev_sb;   /* bd_mapping の host inode の i_sb (src/rt_jbd2.c)。カーネルの blockdev_superblock */
#define sb_is_blkdev_sb(sb) ((sb) == &usbmem_blockdev_sb)
void usbmem_kbh_bdev_attach_host(struct block_device *bd);   /* bd_mapping に host inode を付ける (ext4 のマウント時) */
static inline unsigned long roundup_pow_of_two(unsigned long n) { unsigned long r = 1; while (r < n) r <<= 1; return r; }
#define bh_readahead_batch(n, bhs, flags) do { (void)(n); (void)(bhs); } while (0)
#define folio_trylock(f) (pthread_mutex_trylock(&(f)->lock_.m) == 0 ? ((f)->flags |= 1UL << FOLIO_LOCKED, 1) : 0)
#define kmap_local_bh(bh) ((void *)(bh)->b_data)
#define kunmap_local_bh(bh, p) do { (void)(p); } while (0)
#define filemap_fdatawait_range_keep_errors(m, s, e) (0)
#define bdev_rot(b) (0)
static inline void __clear_bit(unsigned nr, volatile unsigned long *a) { a[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG)); }

/* lockdep / proc / seq_file: 何もしない */
#define rwsem_acquire(...) do { } while (0)
#define rwsem_acquire_read(...) do { } while (0)
#define rwsem_release(...) do { } while (0)
#define lockdep_init_map(...) do { } while (0)
#define lockdep_register_key(k) do { } while (0)
#define lockdep_unregister_key(k) do { } while (0)
#define _THIS_IP_ 0UL
#define mutex_is_locked(m) (1)
#define ZERO_OR_NULL_PTR(p) ((unsigned long)(p) <= 16UL)
#define SLAB_TYPESAFE_BY_RCU 0
#define pr_notice_ratelimited(...) ((void)0)
#define mapping_max_folio_order(m) (0)
#define set_current_state(s) do { } while (0)
#define HRTIMER_MODE_ABS 0
#define schedule_hrtimeout(e, m) (usbmem_jbd2_schedule(), 0)
#define S_IRUGO 0444
struct proc_dir_entry;
struct proc_ops { int (*proc_open)(struct inode *, struct file *); ssize_t (*proc_read)(struct file *, char *, size_t, loff_t *);
  loff_t (*proc_lseek)(struct file *, loff_t, int); int (*proc_release)(struct inode *, struct file *); };
#define proc_mkdir(n, p) ((struct proc_dir_entry *)NULL)
#define proc_create_data(n, m, p, ops, d) ((struct proc_dir_entry *)NULL)
#define remove_proc_entry(n, p) do { } while (0)
static inline int seq_open(struct file *f, const struct seq_operations *op) { (void)f; (void)op; return -ENOSYS; }
static inline int seq_release(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
static inline ssize_t seq_read(struct file *f, char *b, size_t n, loff_t *p) { (void)f; (void)b; (void)n; (void)p; return -ENOSYS; }
static inline loff_t seq_lseek(struct file *f, loff_t o, int w) { (void)f; (void)o; (void)w; return -ENOSYS; }
/* bit waitqueue (jbd2_journal_release_jbd_inode: コミット中の inode を待つ。同期実行なので待つ場面は起きない) */
struct wait_bit_queue_entry { wait_queue_entry_t wq_entry; };
#define DEFINE_WAIT_BIT(name, word, bit) struct wait_bit_queue_entry name = { { .entry = { &name.wq_entry.entry, &name.wq_entry.entry } } }
wait_queue_head_t *usbmem_bit_waitqueue(void *word, int bit);
#define bit_waitqueue(word, bit) usbmem_bit_waitqueue((void *)(word), (bit))
int bmap(struct inode *inode, sector_t *block);   /* a_ops->bmap で論理ブロック → 物理ブロック (0 = 穴) */
void truncate_inode_pages_range(struct address_space *m, loff_t start, loff_t end);

/* ---- per-CPU (CPU は 1 つ) */
#define DEFINE_PER_CPU(type, name) type name
#define this_cpu_read(v) (v)
#define this_cpu_inc(v) ((v)++)
#define this_cpu_add(v, n) ((v) += (n))
#define per_cpu(v, cpu) (v)
#define alloc_percpu(type) ((type *)calloc(1, sizeof(type)))
#define free_percpu(p) free(p)
#define per_cpu_ptr(p, cpu) (p)
#define raw_cpu_ptr(p) (p)
#define this_cpu_ptr(p) (p)
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 1; (cpu)++)
#define num_possible_cpus() 1
#define raw_smp_processor_id() 0
#ifndef smp_processor_id
#define smp_processor_id() 0
#endif

/* ---- atomic / ビット */
#define atomic_sub(n, v) ((void)__atomic_sub_fetch(&(v)->counter, (n), __ATOMIC_SEQ_CST))
#define atomic_inc_return(v) __atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST)
#ifndef atomic_add_return
#define atomic_add_return(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#endif
#define atomic_sub_return(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic64_inc(v) ((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic64_dec(v) ((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
static inline int atomic_dec_if_positive(atomic_t *v) { int c = atomic_read(v); while (c > 0) { if (atomic_cmpxchg(v, c, c - 1) == c) return c - 1; c = atomic_read(v); } return c - 1; }
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define array_index_nospec(i, n) (i)
#define find_next_bit_le(a, size, off) find_next_bit((const unsigned long *)(a), (size), (off))
#define __clear_bit_le(nr, a) __clear_bit((nr), (unsigned long *)(a))
#define WARN_RATELIMIT(c, ...) WARN_ON(c)
#define DEFINE_RAW_FLEX(type, name, member, count) \
  union { u8 bytes[sizeof(type) + sizeof(((type *)0)->member[0]) * (count)]; type obj; } name##_u = { }; \
  type *name = (type *)&name##_u
#define pde_data(i) ((void *)NULL)

/* ---- list (RCU 版は単一スレッドなので通常版) */
static inline void list_splice_init(struct list_head *list, struct list_head *head) {
  if (list_empty(list)) return;
  struct list_head *first = list->next, *last = list->prev, *at = head->next;
  first->prev = head; head->next = first; last->next = at; at->prev = last; INIT_LIST_HEAD(list);
}
static inline void list_splice_tail(struct list_head *list, struct list_head *head) {
  if (list_empty(list)) return;
  struct list_head *first = list->next, *last = list->prev, *at = head->prev;
  first->prev = at; at->next = first; last->next = head; head->prev = last;
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head) { list_splice_tail(list, head); INIT_LIST_HEAD(list); }
static inline void list_replace_init(struct list_head *old, struct list_head *n) {
  n->next = old->next; n->next->prev = n; n->prev = old->prev; n->prev->next = n; INIT_LIST_HEAD(old);
}
#define list_del_rcu(e) list_del(e)
#define list_add_rcu(n, h) list_add((n), (h))
#define list_add_tail_rcu(n, h) list_add_tail((n), (h))
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)

/* ---- xarray (mballoc の「平均断片サイズ別の group 一覧」。添字 = group 番号。src/rt_jbd2.c: 添字順の配列) */
void usbmem_xa_init(struct xarray *xa);
void usbmem_xa_destroy(struct xarray *xa);
int usbmem_xa_empty(const struct xarray *xa);
void *usbmem_xa_load(struct xarray *xa, unsigned long idx);
int usbmem_xa_insert(struct xarray *xa, unsigned long idx, void *entry, gfp_t gfp);
void *usbmem_xa_erase(struct xarray *xa, unsigned long idx);
void *usbmem_xa_find(struct xarray *xa, unsigned long *idx, unsigned long max);
void *usbmem_xa_find_after(struct xarray *xa, unsigned long *idx, unsigned long max);
#define xa_init(xa) usbmem_xa_init(xa)
#define xa_destroy(xa) usbmem_xa_destroy(xa)
#define xa_empty(xa) usbmem_xa_empty(xa)
#define xa_load(xa, i) usbmem_xa_load((xa), (i))
#define xa_insert(xa, i, e, g) usbmem_xa_insert((xa), (i), (e), (g))
#define xa_erase(xa, i) usbmem_xa_erase((xa), (i))
#ifndef xa_lock
#define xa_lock(xa) ((void)(xa))
#define xa_unlock(xa) ((void)(xa))
#endif
#define xa_for_each_range(xa, index, entry, start, last) \
  for ((index) = (start), (entry) = usbmem_xa_find((xa), &(index), (last)); (entry); (entry) = usbmem_xa_find_after((xa), &(index), (last)))
#ifndef xa_for_each
#define xa_for_each(xa, index, entry) xa_for_each_range(xa, index, entry, 0, ~0UL)
#endif

/* ---- ブロックデバイス */
#define set_blocksize(f, size) (0)   /* 外付けジャーナル用 (書き込みでは拒否する) */
int sb_issue_zeroout(struct super_block *sb, sector_t block, sector_t nr, gfp_t gfp);
#define blkdev_issue_zeroout usbmem_kbh_blkdev_issue_zeroout
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp, unsigned flags);
#define getblk_unmovable(bdev, blk, size) __getblk((bdev), (blk), (size))
#define sb_getblk_gfp(sb, blk, gfp) __getblk((sb)->s_bdev, (blk), (unsigned)(sb)->s_blocksize)
#define folio_wait_stable(f) do { } while (0)
#undef bdev_read_only
#define bdev_read_only(b) ((b)->usbmem_ro_)
#define block_page_mkwrite(...) (-ENOSYS)
struct folio *usbmem_write_begin_get_folio(struct address_space *m, pgoff_t idx);
#define write_begin_get_folio(iocb, m, idx, len) usbmem_write_begin_get_folio((m), (idx))
#define filemap_get_folio(m, i) __filemap_get_folio((m), (i), 0, 0)

#ifdef __cplusplus
}
#endif
#endif
