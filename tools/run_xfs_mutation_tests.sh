#!/bin/sh
# XFS 書き込みの安全機構を 1 つずつ外した「突然変異」ビルドを作り、電源断の模擬 (tools/run_xfs_crash_tests.sh) がそれぞれを検出することを確認する。
# 検出 = crash テストが失敗 (終了コード 1) し、失敗の種類 (K/S/A/P) が出ること。1 つでも検出されなければこのスクリプトは失敗する。通常ビルドには入らない。
#   NO_FLUSH_LOG    : ログのレコード (項目) を書いた後、コミットレコードを書く前の flush を省く (順序保証のないデバイスで、コミットが項目より先に残りうる)
#   NO_FLUSH_COMMIT : コミットレコードの後、本来の場所への書き込みの前の flush を省く (コミットがログに残る前に本体が壊れうる)
#   NO_FLUSH_INPLACE: 本来の場所への書き込みの後、unmount レコード (ログを CLEAN にする) の前の flush を省く (CLEAN なのにメタデータが半端)
#   INPLACE_FIRST   : 本来の場所への書き込みをログより先にする (WAL 違反)
#   TAIL_OWN        : 各レコードの tail_lsn を自分の先頭にする (カーネルはトランザクションの先頭側のレコードを再生しない)
#   DATA_AFTER      : ファイルのデータをコミットの後に書く (コミット済みのメタデータが未書き込みのデータを指す窓)
#   NO_CRC          : メタデータの CRC を付けない (カーネルが検出する)
# 使い方: run_xfs_mutation_tests.sh <ビルドディレクトリの親 (例 build/host)> [stride (既定 3)] [変異名...]
set -eu
B=$1; ST=${2:-3}; shift 2 2>/dev/null || shift $#
MUTS=${*:-NO_FLUSH_LOG NO_FLUSH_COMMIT NO_FLUSH_INPLACE INPLACE_FIRST TAIL_OWN DATA_AFTER NO_CRC}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../core" && pwd)
rc=0; found=0; total=0
for m in $MUTS; do
  d="$B/mut_$m"
  mkdir -p "$d"
  cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Release -DUSBMEM_BUILD_TESTS=ON -DUSBMEM_NTFS_WRITE=ON "-DCMAKE_CXX_FLAGS=-DUSBMEM_MUTATE_$m" >/dev/null
  nice -n 10 cmake --build "$d" --target usbmem_xfs_write_tests -j"${USBMEM_JOBS:-2}" >/dev/null 2>"$d/build.err" || { echo "build failed: $m"; tail -5 "$d/build.err"; rc=1; continue; }
  total=$((total + 1))
  set +e
  out=$(sh "$HERE/run_xfs_crash_tests.sh" "$d/compat/usbmem_xfs_write_tests" "$ST" 2>&1)
  r=$?
  set -e
  l=$(printf '%s\n' "$out" | grep -a '^試行' || true)
  if [ $r -eq 0 ]; then
    echo "[$m] ${l:-(試行なし)}"; echo "[$m] 検出されなかった: テストが甘い"; rc=1; continue
  fi
  # 検出と認めるのは、(1) 電源断の試行で K/S/A/P のどれかが 0 より多い、または (2) 試行に入る前の自己検査 (操作列を最後まで実行した結果の検査) が失敗した、のとき。
  # 実行そのものの失敗 (スクリプトが動かない・ツール欠落など) を検出と数えてはいけない。
  if printf '%s' "$l" | grep -aq -E '\((K|S|A|P|KP)\)[^0-9]*[1-9][0-9]* '; then
    echo "[$m] 検出 (電源断の試行): $l"; found=$((found + 1))
  elif printf '%s\n' "$out" | grep -a -q -E 'clean run|final state differs|not clean|log not CLEAN|base image'; then
    echo "[$m] 検出 (自己検査): $(printf '%s\n' "$out" | grep -a -E 'clean run|final state differs|not clean|log not CLEAN|base image' | head -1)"; found=$((found + 1))
  else
    echo "[$m] 失敗したが検出と認められない (スクリプトの不具合?): $(printf '%s\n' "$out" | tail -2 | tr '\n' ' ')"; rc=1
  fi
done
echo "突然変異 $total 種のうち検出 $found"
exit $rc
