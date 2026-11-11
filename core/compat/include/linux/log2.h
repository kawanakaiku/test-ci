#pragma once
#include <compat/kernel.h>
#define ilog2(n) ((unsigned)(63 - __builtin_clzll((unsigned long long)(n))))
static inline unsigned long __roundup_pow_of_two(unsigned long n) { return n <= 1 ? 1 : 1UL << (64 - __builtin_clzl(n - 1)); }
static inline unsigned long __rounddown_pow_of_two(unsigned long n) { return 1UL << (63 - __builtin_clzl(n)); }
#define roundup_pow_of_two(n) __roundup_pow_of_two(n)
#define rounddown_pow_of_two(n) __rounddown_pow_of_two(n)
#define order_base_2(n) ((n) <= 1 ? 0 : ilog2((n) - 1) + 1)
