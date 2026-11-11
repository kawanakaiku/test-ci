/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 同期・原子操作・リスト・ビット操作・メモリ確保の互換 (pthread / GCC builtin ベース)。 */
#ifndef USBMEM_COMPAT_SYNC_H
#define USBMEM_COMPAT_SYNC_H
#include <compat/kernel.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- atomic */
typedef struct { int counter; } atomic_t;
typedef struct { long long counter; } atomic64_t;
#define atomic_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)
#define atomic_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc(v) ((void)__atomic_add_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec(v) ((void)__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST))
#define atomic_dec_return(v) __atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST)
#define atomic_dec_and_test(v) (__atomic_sub_fetch(&(v)->counter, 1, __ATOMIC_SEQ_CST) == 0)
static inline int atomic_cmpxchg(atomic_t *v, int o, int n) {
  __atomic_compare_exchange_n(&v->counter, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return o;
}
#define atomic64_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)
#define atomic64_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic64_add(i, v) ((void)__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic64_sub(i, v) ((void)__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define ATOMIC_INIT(i) { (i) }

/* ---- mutex / rwsem / spinlock / rwlock */
struct mutex { pthread_mutex_t m; };
#define mutex_init(x) pthread_mutex_init(&(x)->m, NULL)
#define mutex_lock(x) pthread_mutex_lock(&(x)->m)
#define mutex_lock_nested(x, sub) pthread_mutex_lock(&(x)->m)
#define mutex_unlock(x) pthread_mutex_unlock(&(x)->m)
#define mutex_trylock(x) (pthread_mutex_trylock(&(x)->m) == 0)

struct rw_semaphore { pthread_rwlock_t l; };
#define init_rwsem(x) pthread_rwlock_init(&(x)->l, NULL)
#define down_read(x) pthread_rwlock_rdlock(&(x)->l)
#define down_write(x) pthread_rwlock_wrlock(&(x)->l)
#define up_read(x) pthread_rwlock_unlock(&(x)->l)
#define up_write(x) pthread_rwlock_unlock(&(x)->l)
#define down_read_trylock(x) (pthread_rwlock_tryrdlock(&(x)->l) == 0)

typedef struct { pthread_mutex_t m; } spinlock_t;
#define spin_lock_init(x) pthread_mutex_init(&(x)->m, NULL)
#define spin_lock(x) pthread_mutex_lock(&(x)->m)
#define spin_unlock(x) pthread_mutex_unlock(&(x)->m)

typedef struct { pthread_rwlock_t l; } rwlock_t;
#define rwlock_init(x) pthread_rwlock_init(&(x)->l, NULL)
#define read_lock_irqsave(x, f) do { (void)(f); pthread_rwlock_rdlock(&(x)->l); } while (0)
#define read_unlock_irqrestore(x, f) pthread_rwlock_unlock(&(x)->l)
#define write_lock_irqsave(x, f) do { (void)(f); pthread_rwlock_wrlock(&(x)->l); } while (0)
#define write_unlock_irqrestore(x, f) pthread_rwlock_unlock(&(x)->l)

#define lockdep_assert_held(x) ((void)0)
#define lockdep_assert_held_write(x) ((void)0)
#define lockdep_off() ((void)0)
#define lockdep_on() ((void)0)
#define lockdep_set_class(l, k) ((void)0)
struct lock_class_key { int unused; };
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define cond_resched() ((void)0)

/* ---- wait queue / completion (Phase 2 は同期実行が前提。待機は条件変数) */
typedef struct { pthread_mutex_t m; pthread_cond_t c; } wait_queue_head_t;
#define init_waitqueue_head(w) do { pthread_mutex_init(&(w)->m, NULL); pthread_cond_init(&(w)->c, NULL); } while (0)
#define wake_up_all(w) do { pthread_mutex_lock(&(w)->m); pthread_cond_broadcast(&(w)->c); pthread_mutex_unlock(&(w)->m); } while (0)
#define wait_event(w, cond) do { pthread_mutex_lock(&(w).m); \
    while (!(cond)) pthread_cond_wait(&(w).c, &(w).m); pthread_mutex_unlock(&(w).m); } while (0)
struct completion { int done; pthread_mutex_t m; pthread_cond_t c; };
#define DECLARE_COMPLETION_ONSTACK(n) struct completion n = { 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
void complete(struct completion *c);
void wait_for_completion(struct completion *c);
#define schedule() sched_yield()

/* ---- list_head */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define LIST_HEAD(n) struct list_head n = LIST_HEAD_INIT(n)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
static inline void list_add_tail(struct list_head *n, struct list_head *h) {
  n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
static inline void list_del(struct list_head *e) {
  e->prev->next = e->next; e->next->prev = e->prev; e->next = e->prev = NULL;
}
#define list_entry(p, t, m) container_of(p, t, m)
#define list_last_entry(h, t, m) list_entry((h)->prev, t, m)
#define list_for_each_entry_safe(pos, n, head, member) \
  for (pos = list_entry((head)->next, __typeof__(*pos), member), \
       n = list_entry(pos->member.next, __typeof__(*pos), member); \
       &pos->member != (head); pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

/* ---- ビット操作 (unsigned long 配列) */
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline void set_bit(unsigned nr, volatile unsigned long *a) { __atomic_fetch_or(&a[nr / BITS_PER_LONG], 1UL << (nr % BITS_PER_LONG), __ATOMIC_SEQ_CST); }
static inline void __set_bit(unsigned nr, volatile unsigned long *a) { a[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG); }
static inline void clear_bit(unsigned nr, volatile unsigned long *a) { __atomic_fetch_and(&a[nr / BITS_PER_LONG], ~(1UL << (nr % BITS_PER_LONG)), __ATOMIC_SEQ_CST); }
static inline int test_bit(unsigned nr, const volatile unsigned long *a) { return (a[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1; }
static inline int test_and_set_bit(unsigned nr, volatile unsigned long *a) {
  unsigned long m = 1UL << (nr % BITS_PER_LONG);
  return (__atomic_fetch_or(&a[nr / BITS_PER_LONG], m, __ATOMIC_SEQ_CST) & m) != 0;
}
static inline int test_and_clear_bit(unsigned nr, volatile unsigned long *a) {
  unsigned long m = 1UL << (nr % BITS_PER_LONG);
  return (__atomic_fetch_and(&a[nr / BITS_PER_LONG], ~m, __ATOMIC_SEQ_CST) & m) != 0;
}
/* x86_64/arm64 (little-endian) では *_le 版は通常版と同一ビット並び */
#define __set_bit_le(nr, a) __set_bit(nr, (unsigned long *)(a))
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off);
#define find_next_zero_bit_le(a, size, off) find_next_zero_bit((const unsigned long *)(a), size, off)
unsigned long bitmap_weight(const unsigned long *a, unsigned int bits);

/* ---- メモリ確保 (GFP フラグは無視) */
#define GFP_KERNEL 0u
#define GFP_NOFS 0u
#define GFP_NOIO 0u
#define GFP_ATOMIC 0u
#define GFP_USER 0u
#define GFP_HIGHUSER 0u
#define GFP_KERNEL_ACCOUNT 0u
#define __GFP_ZERO 1u
#define __GFP_NOWARN 2u
#define __GFP_NOFAIL 4u
/* GFP_* フラグ: __GFP_ZERO のみ解釈する。サイズ計算の桁あふれは NULL を返す。 */
static inline void *usbmem_alloc(size_t n, gfp_t f) { return (f & __GFP_ZERO) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static inline void *usbmem_alloc_array(size_t n, size_t sz, gfp_t f) {
  size_t t; if (__builtin_mul_overflow(n, sz, &t)) return NULL; return usbmem_alloc(t, f);
}
#define kmalloc(n, f) usbmem_alloc((n), (f))
#define kzalloc(n, f) usbmem_alloc((n), (f) | __GFP_ZERO)
#define kmalloc_array(n, s, f) usbmem_alloc_array((n), (s), (f))
#define kfree(p) free(p)
#define kvfree(p) free(p)
#define vfree(p) free(p)
#define kvmalloc(n, f) usbmem_alloc((n), (f))
#define kvzalloc(n, f) usbmem_alloc((n), (f) | __GFP_ZERO)
#define vmalloc(n) usbmem_alloc((n), 0)
#define krealloc(p, n, f) realloc((p), (n))
#define kvrealloc(p, n, f) realloc((p), (n))
#define is_vmalloc_addr(p) 0
char *kstrdup(const char *s, gfp_t g);
void *kmemdup(const void *p, size_t n, gfp_t g);
#define memalloc_nofs_save() 0
#define memalloc_nofs_restore(x) ((void)(x))

struct kmem_cache;
struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags, void (*ctor)(void *));
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t g);
void kmem_cache_free(struct kmem_cache *c, void *p);
void kmem_cache_destroy(struct kmem_cache *c);

/* ---- エラーポインタ / オーバーフロー / ユーティリティ */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long e) { return (void *)e; }
static inline long PTR_ERR(const void *p) { return (long)p; }
static inline int IS_ERR(const void *p) { return IS_ERR_VALUE(p); }
static inline int IS_ERR_OR_NULL(const void *p) { return !p || IS_ERR_VALUE(p); }
static inline void *ERR_CAST(const void *p) { return (void *)p; }
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define check_shl_overflow(a, s, d) ({ \
    __typeof__(*(d)) _r = 0; unsigned long long _a = (unsigned long long)(a); int _s = (int)(s); \
    bool _o = _s < 0 || _s >= (int)(sizeof(_r) * 8); \
    if (!_o) { unsigned long long _v = _a << _s; _r = (__typeof__(*(d)))_v; \
               _o = ((unsigned long long)_r != _v) || ((_v >> _s) != _a); } \
    *(d) = _r; _o; })
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)
#define round_down(x, y) ((x) & ~((__typeof__(x))(y) - 1))
#define ALIGN_DOWN(x, a) round_down(x, a)
#define DIV_ROUND_UP_ULL(n, d) (((n) + (d) - 1) / (d))
#define min3(a, b, c) min(min(a, b), c)
#define cmp_int(a, b) (((a) > (b)) - ((a) < (b)))
#define sizeof_field(t, m) sizeof(((t *)0)->m)
#define struct_size(p, m, n) ({ size_t _s; \
    (__builtin_mul_overflow(sizeof((p)->m[0]), (size_t)(n), &_s) || __builtin_add_overflow(_s, sizeof(*(p)), &_s)) ? (size_t)-1 : _s; })
#define overflows_type(v, t) ({ __typeof__(t) _x; __builtin_add_overflow((v), 0, &_x); })
#define WARN_ON(c) ({ int _w = !!(c); if (_w) fprintf(stderr, "WARN_ON %s:%d\n", __FILE__, __LINE__); _w; })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define BUG_ON(c) do { if (c) { fprintf(stderr, "BUG %s:%d\n", __FILE__, __LINE__); abort(); } } while (0)
#define BUG() abort()
#include <assert.h>
#ifndef static_assert
#define static_assert _Static_assert
#endif
#define panic(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#define unsafe_memcpy(d, s, n, reason) memcpy(d, s, n)
#define le16_add_cpu(p, v) (*(p) = cpu_to_le16(le16_to_cpu(*(p)) + (v)))
#define no_printk(...) ((void)0)
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#define pr_info(...) ((void)0)
#define pr_crit(...) fprintf(stderr, __VA_ARGS__)
#define pr_err_ratelimited(...) fprintf(stderr, __VA_ARGS__)
#define pr_warn_ratelimited(...) ((void)0)
#define str_plural(n) ((n) == 1 ? "" : "s")
size_t strscpy(char *dst, const char *src, size_t n);
char *strreplace(char *s, char from, char to);
#define do_div(n, base) ({ uint32_t _r = (uint32_t)((n) % (base)); (n) /= (base); _r; })
#define div_u64(a, b) ((u64)(a) / (u32)(b))
#define div_s64_rem(a, b, r) ({ s64 _a = (a); *(r) = (s32)(_a % (b)); _a / (b); })

#ifdef __cplusplus
}
#endif
#endif
