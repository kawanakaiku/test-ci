#!/bin/sh
# fs/ntfs を無改変で -fsyntax-only し、不足する互換面を集計する(docs/phase2-spike-result.md 参照)。
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
N=$ROOT/third_party/ntfs-resurrection
SP=${1:-$ROOT/build/spike}
rm -rf "$SP"; mkdir -p "$SP/inc/linux" "$SP/inc/uapi/linux"
for h in $(cat "$N"/fs-ntfs/*.[ch] "$N"/fs-ntfs/lib/* | grep -ho '#include <[^>]*>' | sed 's/#include <\(.*\)>/\1/' | sort -u); do
  mkdir -p "$SP/inc/$(dirname "$h")"; echo '#pragma once' > "$SP/inc/$h"
done
cp "$N/include/uapi/linux/ntfs.h" "$SP/inc/uapi/linux/ntfs.h"
cat > "$SP/inc/linux/types.h" <<'EOT'
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;
typedef uint16_t __be16; typedef uint32_t __be32; typedef uint64_t __be64;
typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64; typedef uint8_t __u8;
typedef int64_t loff_t; typedef uint16_t umode_t; typedef uint64_t sector_t; typedef uint32_t gfp_t;
#define __user
#define __force
#define __bitwise
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)
#define BIT(n) (1UL<<(n))
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define cpu_to_le64(x) ((__le64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define le16_to_cpup(p) (*(p))
#define le32_to_cpup(p) (*(p))
#define le64_to_cpup(p) (*(p))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define __init
#define __exit
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
EOT
for f in "$N"/fs-ntfs/*.c; do
  gcc -fsyntax-only -std=gnu11 -w -I"$SP/inc" -I"$N/fs-ntfs" -include "$SP/inc/linux/types.h" "$f" 2>&1 || true
done > "$SP/errs.txt"
echo "total errors: $(grep -c error "$SP/errs.txt")"
grep -o "implicit declaration of function ‘[^’]*’" "$SP/errs.txt" | sed "s/.*‘\(.*\)’/\1/" | sort -u > "$SP/funcs.txt"
grep -o "‘[^’]*’ undeclared" "$SP/errs.txt" | sed "s/‘\(.*\)’ undeclared/\1/" | sort -u > "$SP/idents.txt"
echo "functions: $(wc -l < "$SP/funcs.txt")  identifiers: $(wc -l < "$SP/idents.txt")"
