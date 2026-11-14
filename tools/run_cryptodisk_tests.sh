#!/bin/sh
# 暗号ボリュームの「ディスク全体」の構成の結合検証。docs/crypto-volumes.md 6.5
# 使い方: run_cryptodisk_tests.sh <usbmem_cryptodisk_tests> <group|all> [fixture_dir]
#   group: gen (フィクスチャを作り直すだけ) / part (パーティション表の中の暗号パーティション) / whole (ディスク全体が暗号) / nested (全体が暗号で中にパーティション表) / errors (異常系)
#   フィクスチャは tools/gen_cryptodisk_fixtures.py が <dir> に作る (無ければここで作る)。必要なツールが無ければ SKIP (終了コード 77)。
set -u
T=$1; G=${2:-all}; D=${3:-/dev/shm/usbmem_cryptodisk_${USER:-u}}
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH="$PATH:/sbin:/usr/sbin"
# 道具が無いとき: 既定は SKIP (77)。USBMEM_CRYPTODISK_REQUIRE=1 (CI) なら失敗にして、見逃しを防ぐ
missing() { if [ -n "${USBMEM_CRYPTODISK_REQUIRE:-}" ]; then echo "FAIL: $1 not available" >&2; exit 1; fi; echo "skip: $1 not available" >&2; exit 77; }
for c in cryptsetup mke2fs mkntfs ntfs-3g fusermount3 sgdisk sfdisk; do
  command -v "$c" >/dev/null 2>&1 || missing "$c"
done
python3 -I -c 'import cryptography, numpy' 2>/dev/null || missing "python3 cryptography/numpy"
[ "$G" = gen ] && rm -f "$D/cases.txt"
if [ ! -f "$D/cases.txt" ]; then
  python3 -I "$HERE/gen_cryptodisk_fixtures.py" "$D" >&2 || { echo "FAIL: fixture generation failed" >&2; exit 1; }
fi
[ "$G" = gen ] && { echo "fixtures ready in $D"; exit 0; }
rc=0; n=0
set -f
while IFS="$(printf '\t')" read -r group name image args; do
  [ "$G" = all ] || [ "$G" = "$group" ] || continue
  n=$((n+1))
  # shellcheck disable=SC2086
  out=$("$T" --scan "$image" $args 2>&1); r=$?
  if [ $r -ne 0 ]; then
    echo "FAIL $group/$name (exit $r)"; echo "$out" | tail -25 | sed 's/^/    /'; rc=1
  else
    echo "ok   $group/$name  $(echo "$out" | tail -1)"
  fi
done < "$D/cases.txt"
[ "$n" -gt 0 ] || { echo "no cases for group $G" >&2; exit 77; }
echo "== $n cases in group $G, rc=$rc"
exit $rc
