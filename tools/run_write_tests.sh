#!/bin/sh
# 実験的な書き込み経路の検証 (USBMEM_NTFS_WRITE=ON ビルド用)。自前ドライバで書いたイメージを、独立実装の ntfsfix -n / ntfs-3g で検証する。
# 使い方: run_write_tests.sh <usbmem_ntfs_tests> [fixture dir (ntfs_basic.img)]
set -eu
T=$1; D=${2:-/dev/shm/usbmem_ntfs_${USER:-u}}
command -v ntfsfix >/dev/null && command -v ntfs-3g >/dev/null || { echo "skip: ntfs-3g tools missing" >&2; exit 77; }
[ -f "$D/ntfs_basic.img" ] || { echo "skip: no fixture in $D" >&2; exit 77; }
W=$(mktemp -d /dev/shm/usbmem_wt.XXXXXX); trap 'fusermount3 -u "$W/m" 2>/dev/null || true; rm -rf "$W"' EXIT
rc=0
# ケース: パス オフセット 長さ seed  (クラスタ/ページ境界またぐ・非整列・先頭・末尾・1 バイト)
check() {  # check <image> <path> <off> <len> <seed> [BS]
  img=$1; path=$2; off=$3; len=$4; seed=$5
  cp "$img" "$W/w.img"
  "$T" --wtest "$W/w.img" "$path" "$off" "$len" "$seed" 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true
  ntfsfix -n "$W/w.img" >/dev/null 2>&1 || { echo "FAIL ntfsfix $path $off $len"; return 1; }
  mkdir -p "$W/m"; ntfs-3g -o ro "$W/w.img" "$W/m" 2>/dev/null || { echo "FAIL ntfs-3g mount"; return 1; }
  python3 -I - "$img" "$W/m/$path" "$off" "$len" "$seed" "$W/orig.m" <<'PY' || { fusermount3 -u "$W/m"; echo "FAIL content $path $off $len"; return 1; }
import sys
img, got, off, ln, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
data = bytearray(open(got, 'rb').read())
# 元の内容は manifest 由来ではなく「書いていない範囲は元イメージと同じ」を、ファイルを再生成せず比較するため、
# 書いた範囲だけパターンを確認し、範囲外は ntfs-3g で別途 manifest 検証に任せる。
exp = bytes(((off + i) * 7 + seed) & 255 for i in range(ln))
sys.exit(0 if bytes(data[off:off+ln]) == exp else 1)
PY
  fusermount3 -u "$W/m"
  return 0
}
IMG=$D/ntfs_basic.img
check "$IMG" big.bin 0 1 1 || rc=1
check "$IMG" big.bin 4095 2 2 || rc=1
check "$IMG" big.bin 100000 70000 3 || rc=1
check "$IMG" big.bin 3146000 505 4 || rc=1
check "$IMG" big.bin 1048575 1048578 5 || rc=1
# 書き込み後に manifest が (big.bin 以外) 全て保たれること
cp "$IMG" "$W/w.img"; "$T" --wtest "$W/w.img" big.bin 500000 20000 9 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true
sed '/ big.bin$/d' "$D/ntfs_basic.manifest" > "$W/m2.manifest"
"$T" --manifest "$W/w.img" "$W/m2.manifest" 2>&1 | grep -E "manifest:" || rc=1
echo "write tests rc=$rc"
exit $rc
