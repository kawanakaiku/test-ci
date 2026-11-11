#pragma once
#include <compat/kernel.h>
/* ブロックグループごとのスピンロック (ext2/ext4 の割り当てビットマップ用)。ホストは単純な固定配列。 */
#define NR_BG_LOCKS 128
struct bgl_lock { spinlock_t lock; };
struct blockgroup_lock { struct bgl_lock locks[NR_BG_LOCKS]; };
static inline void bgl_lock_init(struct blockgroup_lock *bgl) { for (int i = 0; i < NR_BG_LOCKS; i++) spin_lock_init(&bgl->locks[i].lock); }
static inline spinlock_t *bgl_lock_ptr(struct blockgroup_lock *bgl, unsigned int block_group) { return &bgl->locks[block_group % NR_BG_LOCKS].lock; }
