#!/bin/sh
# 実験的な書き込み経路の検証 (USBMEM_NTFS_WRITE=ON ビルド用)。自前ドライバで書いたイメージを、独立実装の ntfsfix -n / ntfs-3g で検証する。
# 期待値は「元イメージのファイル内容 + 書いた範囲のパターン (+ 拡張時の隙間はゼロ)」を計算し、ファイル全体を比較する。他ファイルは manifest で確認。
# 使い方: run_write_tests.sh <usbmem_ntfs_tests> [fixture dir (ntfs_basic.img)]
set -eu
T=$1; D=${2:-/dev/shm/usbmem_ntfs_${USER:-u}}
command -v ntfsfix >/dev/null && command -v ntfs-3g >/dev/null || { echo "skip: ntfs-3g tools missing" >&2; exit 77; }
[ -f "$D/ntfs_basic.img" ] || { echo "skip: no fixture in $D" >&2; exit 77; }
W=$(mktemp -d /dev/shm/usbmem_wt.XXXXXX); trap 'fusermount3 -u "$W/m" 2>/dev/null || true; fusermount3 -u "$W/o" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir -p "$W/m" "$W/o"
IMG=$D/ntfs_basic.img
rc=0
ntfs-3g -o ro "$IMG" "$W/o" 2>/dev/null || { echo "cannot mount original"; exit 1; }
check() {  # check <mode: inplace|iter> <path> <off> <len> <seed>
  mode=$1; path=$2; off=$3; len=$4; seed=$5
  cp "$IMG" "$W/w.img"
  if [ "$mode" = iter ]; then export USBMEM_WRITE_ITER=1; else unset USBMEM_WRITE_ITER || true; fi
  out=$("$T" --wtest "$W/w.img" "$path" "$off" "$len" "$seed" 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true)
  [ -z "$out" ] || { echo "FAIL[$mode $path $off $len] driver: $out" | cut -c1-200; return 1; }
  ntfsfix -n "$W/w.img" >/dev/null 2>&1 || { echo "FAIL[$mode $path $off $len] ntfsfix"; return 1; }
  ntfs-3g -o ro "$W/w.img" "$W/m" 2>/dev/null || { echo "FAIL[$mode $path $off $len] ntfs-3g mount"; return 1; }
  python3 -I - "$W/o/$path" "$W/m/$path" "$off" "$len" "$seed" <<'PY' || { fusermount3 -u "$W/m"; echo "FAIL[$mode $path $off $len] content"; return 1; }
import sys, os
orig, got, off, ln, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
exp = bytearray(open(orig, 'rb').read()) if os.path.exists(orig) else bytearray()
if len(exp) < off + ln: exp += bytes(off + ln - len(exp))
for i in range(off, off + ln): exp[i] = ((i * 7) + seed) & 255
data = open(got, 'rb').read()
if data != bytes(exp):
    d = next((i for i in range(min(len(data), len(exp))) if data[i] != exp[i]), None)
    print("size got/exp:", len(data), len(exp), "first diff:", d); sys.exit(1)
PY
  fusermount3 -u "$W/m"
  return 0
}
# --- 既存範囲の上書き (write_inplace と write_iter): 1 バイト / ページ境界 / 複数ページ / EOF 直前
check inplace big.bin 0 1 1 || rc=1
check inplace big.bin 100000 70000 3 || rc=1
check iter big.bin 0 1 1 || rc=1
check iter big.bin 4095 2 2 || rc=1
check iter big.bin 100000 70000 3 || rc=1
check iter big.bin 1048575 1048578 5 || rc=1
# --- 拡張: EOF 直前から / EOF ちょうどから / 隙間 (ゼロ埋め) / ページ・クラスタ境界 / 数 MiB
check iter big.bin 3146000 5000 4 || rc=1
check iter big.bin 3146505 100 6 || rc=1
check iter big.bin 3200000 1000 7 || rc=1
check iter big.bin 4194304 4096 8 || rc=1
check iter big.bin 3146505 3000000 9 || rc=1
# --- 小さな常駐ファイル: 内部上書き / 常駐のまま拡張 / 非常駐へ昇格 / 空ファイルへ書く / 隙間付き
check iter a.txt 0 3 10 || rc=1
check iter a.txt 6 50 11 || rc=1
check iter a.txt 0 5000 12 || rc=1
check iter empty.txt 0 10 13 || rc=1
check iter empty.txt 0 20000 14 || rc=1
check iter a.txt 9000 100 15 || rc=1
# --- 書き込み後に manifest が (書いたファイル以外) 全て保たれること
cp "$IMG" "$W/w.img"; USBMEM_WRITE_ITER=1 "$T" --wtest "$W/w.img" big.bin 500000 20000 9 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true
sed '/ big.bin$/d' "$D/ntfs_basic.manifest" > "$W/m2.manifest"
"$T" --manifest "$W/w.img" "$W/m2.manifest" 2>&1 | grep -E "manifest:" | grep -q " 0 failures" || { echo "FAIL manifest after write"; rc=1; }
echo "write tests rc=$rc"
exit $rc
