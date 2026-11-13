#!/bin/sh
# FAT 書き込みの突然変異テスト (docs/fat-write.md): 安全の規則を 1 つずつ外したビルドを作り、tools/run_fat_crash_tests.sh がそれを**検出して失敗する**ことを確かめる
# (検出できない = 電源断のテストが規則を守らせる力を持っていない)。通常のビルドには入らない (別のビルドディレクトリ・-DUSBMEM_MUTATE_...)。
# 使い方: run_fat_mutation_tests.sh [サンプル数 (既定 20)] [突然変異名...]   既定: ORDER NO_DIRTY NO_KIND_SYNC NO_NEWCLU_FIRST NO_DATA_FIRST LOG_REVERSE NO_RENAME_BOUNDARY NO_DIRGROW_BOUNDARY
#   環境変数: USBMEM_MUT_BUILD (ビルド置き場。既定 /dev/shm/usbmem_fat_mut_$USER)、MUT_TYPES (既定 "12 16 32")
# 終了コード: 0 = 全部検出、1 = 検出できなかった突然変異がある、77 = 道具が無い
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$HERE/..
NS=${1:-20}; [ $# -gt 0 ] && shift
MUTS=${*:-"ORDER NO_DIRTY NO_KIND_SYNC NO_NEWCLU_FIRST NO_DATA_FIRST LOG_REVERSE NO_RENAME_BOUNDARY NO_DIRGROW_BOUNDARY"}
for c in cmake ninja fsck.fat mkfs.fat mcopy 7z; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
B=${USBMEM_MUT_BUILD:-/dev/shm/usbmem_fat_mut_${USER:-u}}
mkdir -p "$B"
rc=0
for m in $MUTS; do
  cmake -S "$ROOT/core" -B "$B/$m" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DUSBMEM_NTFS_WRITE=ON "-DCMAKE_C_FLAGS=-DUSBMEM_MUTATE_$m" >"$B/$m.cfg.log" 2>&1 || { echo "FAIL: cmake $m"; rc=1; continue; }
  cmake --build "$B/$m" --target usbmem_fat_write_tests >"$B/$m.build.log" 2>&1 || { tail -5 "$B/$m.build.log"; echo "FAIL: build $m"; rc=1; continue; }
  if sh "$HERE/run_fat_crash_tests.sh" "$B/$m/compat/usbmem_fat_write_tests" x "$NS" "${MUT_TYPES:-12 16 32}" >"$B/$m.log" 2>&1; then
    echo "NOT DETECTED: USBMEM_MUTATE_$m (the crash tests passed with the rule removed)"; rc=1
  else
    echo "detected: USBMEM_MUTATE_$m: $(grep -a '^合計' "$B/$m.log" | tail -1)"
  fi
done
[ $rc = 0 ] && echo "fat mutation tests: all mutations detected"
exit $rc
