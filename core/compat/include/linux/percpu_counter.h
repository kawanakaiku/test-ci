#pragma once
#include <compat/kernel.h>
/* percpu_counter: ホストはスレッド安全が要らない用途 (FS ごとにロックされる) なので単なる s64。 */
struct percpu_counter { s64 count; };
static inline int percpu_counter_init(struct percpu_counter *c, s64 v, gfp_t gfp) { (void)gfp; c->count = v; return 0; }
static inline void percpu_counter_destroy(struct percpu_counter *c) { (void)c; }
static inline void percpu_counter_set(struct percpu_counter *c, s64 v) { c->count = v; }
static inline s64 percpu_counter_read(struct percpu_counter *c) { return c->count; }
static inline s64 percpu_counter_read_positive(struct percpu_counter *c) { return c->count > 0 ? c->count : 0; }
static inline s64 percpu_counter_sum(struct percpu_counter *c) { return c->count; }
static inline s64 percpu_counter_sum_positive(struct percpu_counter *c) { return c->count > 0 ? c->count : 0; }
static inline void percpu_counter_add(struct percpu_counter *c, s64 a) { c->count += a; }
static inline void percpu_counter_inc(struct percpu_counter *c) { c->count++; }
static inline void percpu_counter_dec(struct percpu_counter *c) { c->count--; }
static inline void percpu_counter_sub(struct percpu_counter *c, s64 a) { c->count -= a; }
