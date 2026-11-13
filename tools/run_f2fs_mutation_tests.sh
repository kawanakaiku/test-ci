#!/bin/sh
# F2FS 書き込みの「安全機構を壊した版」で電源断テストを実行し、テストがそれを検出する (= 失敗する) ことを確認する (ミューテーションテスト。docs/f2fs-write.md)。
# 使い方: run_f2fs_mutation_tests.sh <USBMEM_NTFS_WRITE=ON でビルド済みの build dir (ninja)> [サンプル数 (既定 14)] [ミューテーション名 ...]
#   ミューテーション (core/compat/src/f2fs_write*.inc の #ifdef):
#     USBMEM_MUTATE_F2FS_CP_SAME_PACK   新しい CP パックを現在有効な側に書く (反対側へ交互に書かない)
#     USBMEM_MUTATE_F2FS_NO_FLUSH       CP の本体と末尾 (footer) の間の flush を省く
#     USBMEM_MUTATE_F2FS_FOOTER_FIRST   CP の末尾 (footer) を本体より先に書く
#     USBMEM_MUTATE_F2FS_REUSE_FREED    解放して次の CP でコミットされる前のセグメントを再利用する
#     USBMEM_MUTATE_F2FS_NAT_INPLACE    NAT ブロックを交互のコピーではなくその場で書き換える
#     USBMEM_MUTATE_F2FS_SIT_INPLACE    SIT ブロックを交互のコピーではなくその場で書き換える
#     USBMEM_MUTATE_F2FS_CP_VER_SAME    新しい CP のバージョン番号を増やさない
# 終了コード: 0 = すべて検出された / 1 = 検出されなかったミューテーションがある / 77 = 道具が無い。
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
B=$1; NS=${2:-14}; shift 2 || shift $#
MUTS=${*:-"USBMEM_MUTATE_F2FS_CP_SAME_PACK USBMEM_MUTATE_F2FS_NO_FLUSH USBMEM_MUTATE_F2FS_FOOTER_FIRST USBMEM_MUTATE_F2FS_REUSE_FREED USBMEM_MUTATE_F2FS_NAT_INPLACE USBMEM_MUTATE_F2FS_SIT_INPLACE USBMEM_MUTATE_F2FS_CP_VER_SAME"}
command -v ninja >/dev/null || { echo "skip: ninja missing" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_f2m.XXXXXX)
trap 'rm -rf "$W"' EXIT
OBJ=compat/CMakeFiles/usbmem_f2fs.dir/src/f2fs_volume.cpp.o
CC_CMD=$(ninja -C "$B" -t commands "$OBJ" | tail -1)
LINK_CMD=$(ninja -C "$B" -t commands compat/usbmem_f2fs_write_tests | tail -1)
export USBMEM_F2FS_KERNEL=off
undetected=0; detected=0
for m in $MUTS; do
  cmd=$(printf "%s" "$CC_CMD" | sed -e "s| -o $OBJ | -o $W/mut.o |" -e "s| -c | -D$m=1 -c |")
  (cd "$B" && sh -c "$cmd") > "$W/cc.log" 2>&1 || { tail -5 "$W/cc.log"; echo "compile failed: $m"; exit 1; }
  lcmd=$(printf "%s" "$LINK_CMD" | sed -e "s| -o compat/usbmem_f2fs_write_tests | -o $W/t_mut |" -e "s| compat/libusbmem_f2fs.a | $W/mut.o compat/libusbmem_f2fs.a |" -e "s|^: && ||" -e "s| && :\$||")
  (cd "$B" && sh -c "$lcmd") > "$W/ld.log" 2>&1 || { tail -5 "$W/ld.log"; echo "link failed: $m"; exit 1; }
  if sh "$HERE/run_f2fs_crash_tests.sh" "$W/t_mut" "$NS" > "$W/run.log" 2>&1; then
    echo "NOT DETECTED: $m"; undetected=$((undetected + 1))
  else
    echo "detected: $m ($(grep -a '試行' "$W/run.log" | head -1 | cut -c1-150))"; detected=$((detected + 1))
  fi
done
echo "ミューテーション: 検出 $detected / 未検出 $undetected"
[ $undetected -eq 0 ]
