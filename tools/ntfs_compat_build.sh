#!/bin/sh
# 互換レイヤーに対して fs/ntfs の各 .c を -fsyntax-only し、固有エラーを要約する(開発用)。
ROOT=$(cd "$(dirname "$0")/.." && pwd)
N=$ROOT/third_party/ntfs-resurrection
OUT=${1:-$ROOT/build/compat-errs.txt}
mkdir -p "$(dirname "$OUT")"
for f in "$N"/fs-ntfs/*.c; do
  gcc -fsyntax-only -std=gnu11 -w -I"$ROOT/core/compat/include" -I"$N/include" -iquote "$N/fs-ntfs" "$f" 2>&1
done > "$OUT"
echo "errors: $(grep -c 'error' "$OUT")"
for k in "unknown type name" "undeclared" "implicit declaration" "undefined type" "has no member"; do
  printf '%-22s %s\n' "$k" "$(grep "$k" "$OUT" | wc -l)"
done
