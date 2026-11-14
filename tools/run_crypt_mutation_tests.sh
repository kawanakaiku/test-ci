#!/bin/bash
# 暗号ボリューム書き込みの安全機構を 1 つずつ外した「突然変異」ビルドを作り、単体テスト (usbmem_transform_tests) または独立検証 (tools/run_crypt_write_tests.sh) がそれぞれを検出することを確認する。
# 1 つでも検出されなければこのスクリプトは失敗する。変異は -DUSBMEM_MUTATE_<名前> で有効になり、通常ビルドには入らない。
#   CW_NO_CACHE_UPDATE   : 書き込み後にページキャッシュを更新しない (古い平文が読める)
#   CW_NO_RMW_HEAD       : 先頭の部分セクタを読み込まずに暗号化する (同じセクタの残りが壊れる)
#   CW_NO_FLUSH          : flush を下位へ渡さない
#   CW_NO_LATCH          : 下位の書き込み失敗で読み取り専用に落とさない
#   CW_ALWAYS_WRITABLE   : open 時の明示 (writable) を無視して常に書き込み可にする
#   CW_LUKS_IV           : LUKS の IV (セクタ番号) が 1 ずれる
#   CW_BL_NO_VH_MAP      : BitLocker の先頭セクタ群を退避先へ写さない
#   CW_BL_NO_HIDDEN_CHECK: BitLocker のメタデータ領域への書き込みを拒否しない
#   CW_BL_PARTIAL        : 変換途中の BitLocker にも書き込む
#   CW_DIFF_ORDER        : Elephant diffuser の逆変換の順序が逆
# 使い方: run_crypt_mutation_tests.sh <ビルドディレクトリの親> [変異名...]
set -u
B=$1; shift
MUTS=${*:-CW_NO_CACHE_UPDATE CW_NO_RMW_HEAD CW_NO_FLUSH CW_NO_LATCH CW_ALWAYS_WRITABLE CW_LUKS_IV CW_BL_NO_VH_MAP CW_BL_NO_HIDDEN_CHECK CW_BL_PARTIAL CW_DIFF_ORDER}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../core" && pwd)
rc=0; found=0; total=0
for m in $MUTS; do
  d="$B/mut_$m"
  mkdir -p "$d"
  cmake -S "$SRC" -B "$d" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSBMEM_BUILD_TESTS=ON -DUSBMEM_NTFS=OFF -DUSBMEM_CRYPT_WRITE=ON "-DCMAKE_CXX_FLAGS=-DUSBMEM_MUTATE_$m" >/dev/null || { echo "configure failed: $m"; rc=1; continue; }
  nice -n 10 cmake --build "$d" --target usbmem_transform_tests usbmem_cryptwrite_tool -j"${USBMEM_JOBS:-2}" >/dev/null 2>"$d/build.err" || { echo "build failed: $m"; tail -5 "$d/build.err"; rc=1; continue; }
  total=$((total + 1))
  if out=$("$d/usbmem_transform_tests" 2>&1); then
    only=luks; case $m in CW_BL_*|CW_DIFF_*|CW_ALWAYS_*) only=bl ;; esac
    out=$(USBMEM_CW_ONLY=$only USBMEM_CW_OPS=${USBMEM_CW_OPS:-150} USBMEM_CW_SEEDS=1 USBMEM_CW_FUZZ=20 bash "$HERE/run_crypt_write_tests.sh" "$d/usbmem_cryptwrite_tool" "$d/work" 2>&1); r=$?
    if [ $r -eq 0 ]; then echo "[$m] 検出されなかった (単体テストも独立検証も通った)"; rc=1
    elif [ $r -eq 77 ]; then echo "[$m] 独立検証の道具が無く判定できない"; rc=1
    else echo "[$m] 検出 (独立検証): $(printf '%s\n' "$out" | grep -a '^FAIL' | head -2 | tr '\n' ' ')"; found=$((found + 1)); fi
  else
    echo "[$m] 検出 (単体テスト): $(printf '%s\n' "$out" | grep -a -E '^(FAIL|CHECK)|failed' | head -2 | tr '\n' ' ')"; found=$((found + 1))
  fi
  rm -rf "$d/work"
done
echo "突然変異 $total 種のうち検出 $found"
exit $rc
