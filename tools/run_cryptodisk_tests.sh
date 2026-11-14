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
# 並列度: USBMEM_CRYPTODISK_JOBS (既定 1。sanitizer ビルドは 1 件が遅いので CI は 3)。結果は番号順に出す
J=${USBMEM_CRYPTODISK_JOBS:-1}
R=$(mktemp -d "${TMPDIR:-/tmp}/cryptodisk_run.XXXXXX") || exit 1
trap 'rm -rf "$R"' EXIT
n=0; running=0
set -f
while IFS="$(printf '\t')" read -r group name image args; do
  [ "$G" = all ] || [ "$G" = "$group" ] || continue
  n=$((n+1))
  echo "$group/$name" > "$R/$n.name"
  # shellcheck disable=SC2086
  ( "$T" --scan "$image" $args > "$R/$n.out" 2>&1; echo $? > "$R/$n.rc" ) &
  running=$((running+1))
  if [ "$running" -ge "$J" ]; then wait; running=0; fi
done < "$D/cases.txt"
wait
[ "$n" -gt 0 ] || { echo "no cases for group $G" >&2; exit 77; }
rc=0; i=1
while [ "$i" -le "$n" ]; do
  r=$(cat "$R/$i.rc" 2>/dev/null || echo 255); nm=$(cat "$R/$i.name")
  if [ "$r" -ne 0 ]; then
    echo "FAIL $nm (exit $r)"; tail -25 "$R/$i.out" | sed 's/^/    /'; rc=1
  else
    echo "ok   $nm  $(tail -1 "$R/$i.out")"
  fi
  i=$((i+1))
done
echo "== $n cases in group $G, rc=$rc"
exit $rc
