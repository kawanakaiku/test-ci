#pragma once
#include <compat/kernel.h>
/* mbcache (xattr ブロックの共有)。読み取り専用では共有の探索は不要なので、何も共有しない空実装。 */
struct mb_cache; struct mb_cache_entry { u32 e_key; u64 e_value; bool e_reusable; };
static inline struct mb_cache *mb_cache_create(int bucket_bits) { (void)bucket_bits; return (struct mb_cache *)1; }
static inline void mb_cache_destroy(struct mb_cache *c) { (void)c; }
static inline int mb_cache_entry_create(struct mb_cache *c, gfp_t f, u32 key, u64 value, bool reusable) { (void)c; (void)f; (void)key; (void)value; (void)reusable; return 0; }
static inline void mb_cache_entry_delete_or_get(struct mb_cache *c, u32 key, u64 value) { (void)c; (void)key; (void)value; }
static inline struct mb_cache_entry *mb_cache_entry_find_first(struct mb_cache *c, u32 key) { (void)c; (void)key; return NULL; }
static inline struct mb_cache_entry *mb_cache_entry_find_next(struct mb_cache *c, struct mb_cache_entry *e) { (void)c; (void)e; return NULL; }
static inline struct mb_cache_entry *mb_cache_entry_get(struct mb_cache *c, u32 key, u64 value) { (void)c; (void)key; (void)value; return NULL; }
static inline void mb_cache_entry_touch(struct mb_cache *c, struct mb_cache_entry *e) { (void)c; (void)e; }
static inline void mb_cache_entry_put(struct mb_cache *c, struct mb_cache_entry *e) { (void)c; (void)e; }
