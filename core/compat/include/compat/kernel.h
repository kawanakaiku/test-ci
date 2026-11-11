/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LinuxCompat: Linux カーネル API のユーザー空間互換レイヤー(共通)。
 * ファイルシステムごとに複製せず、ここに集約する。fs/ntfs など上流コードは無改変で使う。
 * 設計: docs/phase2-spike-result.md
 */
#ifndef USBMEM_COMPAT_KERNEL_H
#define USBMEM_COMPAT_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#ifndef EFSCORRUPTED
#define EFSCORRUPTED EUCLEAN   /* カーネル定義: ファイルシステム破損 */
#endif
#ifndef EFSBADCRC
#define EFSBADCRC EBADMSG
#endif
#include <strings.h>   /* ffs (bionic は string.h 経由では宣言しない) */

/* ---- 基本型 */
#ifdef __ANDROID__
/* bionic は uapi の <linux/types.h> (__u64 = unsigned long long 等) を内部で使うので、それに合わせる。
 * 実カーネルの u64 も unsigned long long なので、むしろ上流に忠実。 */
#include <linux/types.h>
typedef __u8 u8; typedef __u16 u16; typedef __u32 u32; typedef __u64 u64;
typedef __s8 s8; typedef __s16 s16; typedef __s32 s32; typedef __s64 s64;
#else
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;
typedef uint16_t __be16; typedef uint32_t __be32; typedef uint64_t __be64;
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef int8_t __s8; typedef int16_t __s16; typedef int32_t __s32; typedef int64_t __s64;
typedef uint64_t __aligned_u64 __attribute__((aligned(8)));
#endif
#ifndef __ANDROID__
typedef int64_t loff_t;
#endif
typedef uint16_t umode_t;
typedef uint64_t sector_t;
typedef unsigned long pgoff_t;
typedef uint32_t gfp_t;
typedef int vm_fault_t;
typedef uint32_t blk_opf_t;
typedef unsigned int fmode_t;

/* ---- 属性/修飾子 */
#define __user
#define __force
#define __bitwise
#define __rcu
#define __init
#define __exit
#define __must_check
#define __always_inline inline __attribute__((always_inline))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define __maybe_unused __attribute__((unused))
#define noinline __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define fallthrough __attribute__((fallthrough))
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

/* ---- 汎用マクロ */
#define BIT(n) (1UL << (n))
#define BITS_PER_LONG (sizeof(long) * 8)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define swap(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON")
#define offsetofend(t, m) (offsetof(t, m) + sizeof(((t *)0)->m))
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define SZ_1K 0x400
#define SZ_4K 0x1000
#define SZ_64K 0x10000
#define U64_MAX UINT64_MAX
#define U32_MAX UINT32_MAX
#define S64_MAX INT64_MAX
#define LLONG_MAX_ INT64_MAX
#define MAX_LFS_FILESIZE ((loff_t)INT64_MAX)

/* ---- エンディアン (対象は little-endian のみ: x86_64 / arm64 Android) */
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "big-endian is not supported"
#endif
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define le16_to_cpup(p) (*(const u16 *)(p))
#define le32_to_cpup(p) (*(const u32 *)(p))
#define le64_to_cpup(p) (*(const u64 *)(p))
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be32_to_cpu(x) __builtin_bswap32(x)
#define get_unaligned_le16(p) ({ u16 _v; memcpy(&_v, (p), 2); _v; })
#define get_unaligned_le32(p) ({ u32 _v; memcpy(&_v, (p), 4); _v; })
#define get_unaligned_le64(p) ({ u64 _v; memcpy(&_v, (p), 8); _v; })
#define put_unaligned_le16(v, p) do { u16 _v = (v); memcpy((p), &_v, 2); } while (0)
#define put_unaligned_le32(v, p) do { u32 _v = (v); memcpy((p), &_v, 4); } while (0)
#define put_unaligned_le64(v, p) do { u64 _v = (v); memcpy((p), &_v, 8); } while (0)
#define get_unaligned(p) ({ __typeof__(*(p)) _v; memcpy(&_v, (p), sizeof _v); _v; })
#define put_unaligned(v, p) do { __typeof__(*(p)) _v = (v); memcpy((p), &_v, sizeof _v); } while (0)

#include <compat/sync.h>
#include <compat/fs.h>
#include <compat/consts.h>
#include <compat/vfs_api.h>
#include <compat/misc.h>

#endif
