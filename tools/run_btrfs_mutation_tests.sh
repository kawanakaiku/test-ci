#!/bin/sh
# Btrfs 書き込みの安全機構を 1 つずつ外した「突然変異」ビルドを作り、電源断の模擬 (tools/run_btrfs_crash_tests.sh) がそれぞれを検出することを確認する。
# 検出 = crash テストが失敗 (終了コード 1) し、失敗の種類 (K/S/A/P) が出ること。1 つでも検出されなければこのスクリプトは失敗する。通常ビルドには入らない。
#   NO_FLUSH     : ツリーブロック・データの書き出しの後、スーパーブロックを書く前の flush を省く
#   NO_SB_WRITE  : スーパーブロックを書かない (コミットが確定しない)
#   DATA_AFTER   : ファイルのデータをスーパーブロックの確定の後に書く
#   REUSE_FREED  : 解放したブロックを同じ世代のうちに再利用する
# 使い方: run_btrfs_mutation_tests.sh <ビルドディレクトリの親 (例 build/host)> [サンプル数 (既定 30)] [変異名...]
set -eu
B=$1; NS=${2:-30}; shift 2 2>/dev/null || shift $#
MUTS=${*:-NO_FLUSH NO_SB_WRITE DATA_AFTER REUSE_FREED}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../core" && pwd)
rc=0; found=0; total=0
for m in $MUTS; do
  d="$B/mut_$m"
  cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DUSBMEM_BUILD_TESTS=ON -DUSBMEM_NTFS_WRITE=ON "-DCMAKE_CXX_FLAGS=-DUSBMEM_MUTATE_$m" >/dev/null
  nice -n 10 cmake --build "$d" --target usbmem_btrfs_write_tests -j"${USBMEM_JOBS:-2}" >/dev/null 2>"$d/build.err" || { echo "build failed: $m"; tail -5 "$d/build.err"; rc=1; continue; }
  total=$((total + 1))
  set +e
  out=$("$HERE/run_btrfs_crash_tests.sh" "$d/compat/usbmem_btrfs_write_tests" "$NS" 2>&1)
  r=$?
  set -e
  l=$(printf '%s\n' "$out" | grep -a '^試行' || true)
  [ -n "$l" ] || l="(電源断の試行に入る前の自己検査で失敗) $(printf '%s\n' "$out" | tail -1)"
  echo "[$m] $l"
  if [ $r -eq 0 ]; then echo "[$m] 検出されなかった: テストが甘い"; rc=1; else found=$((found + 1)); fi
done
echo "突然変異 $total 種のうち検出 $found"
exit $rc
