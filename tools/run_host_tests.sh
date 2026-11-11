#!/bin/sh
# PC 上でネイティブコアのテストを実行する。通常ビルドと ASan+UBSan ビルドの両方。
# 使い方: tools/run_host_tests.sh [ビルド出力先(既定: build/host)]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/host}
# 一部環境では /etc/ld.so.preload の影響で ASan のリンク順チェックが誤検知するため無効化する
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
for mode in plain san; do
  flags=""
  [ "$mode" = san ] && flags="-DUSBMEM_SANITIZE=ON"
  cmake -S "$ROOT/core" -B "$OUT/$mode" -DCMAKE_BUILD_TYPE=Debug $flags >/dev/null
  cmake --build "$OUT/$mode" -j"$(nproc)"
  (cd "$OUT/$mode" && ctest --output-on-failure)
done
