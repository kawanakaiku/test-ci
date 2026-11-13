/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fs/jbd2 (revoke.c) 用: hash_64 (include/linux/hash.h と同じ乗数)。hash_32 は compat/more.h にある */
#pragma once
#include <compat/kernel.h>
#define GOLDEN_RATIO_64 0x61C8864680B583EBull
static inline u32 hash_64(u64 val, unsigned int bits) { return bits ? (u32)((val * GOLDEN_RATIO_64) >> (64 - bits)) : 0; }
