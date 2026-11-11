#!/bin/sh
# 実際の mkntfs/ntfs-3g で NTFS テストイメージを tmpfs に作る。出力: <dir>/ntfs_basic.img と ntfs_basic.manifest
# manifest 形式: "<種別 f|d> <サイズ> <sha256|-> <パス>" (パスは行末まで)
# 使い方: gen_ntfs_fixture.sh [出力ディレクトリ(既定 /dev/shm/usbmem_ntfs_$USER)]
set -eu
OUT=${1:-/dev/shm/usbmem_ntfs_$USER}
MKNTFS=$(command -v mkntfs || echo /sbin/mkntfs)
# 必要ツールが無い環境では CTest 上 SKIP (終了コード 77)
{ [ -x "$MKNTFS" ] && command -v ntfs-3g >/dev/null && command -v fusermount3 >/dev/null; } || { echo "skip: mkntfs/ntfs-3g/fusermount3 not available" >&2; exit 77; }
mkdir -p "$OUT"; IMG=$OUT/ntfs_basic.img; MNT=$OUT/mnt.$$
rm -f "$IMG"; truncate -s 32M "$IMG"
"$MKNTFS" -F -Q -q -s 512 -c 4096 -L TESTVOL "$IMG" >/dev/null 2>&1
mkdir -p "$MNT"; ntfs-3g -o rw "$IMG" "$MNT"
trap 'fusermount3 -u "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null' EXIT
printf 'hello\n' > "$MNT/a.txt"
mkdir "$MNT/dir1" "$MNT/dir1/sub"
printf 'deep\n' > "$MNT/dir1/b.txt"
printf '\343\203\225\343\202\241\343\202\244\343\203\253\n' > "$MNT/日本語.txt"
python3 -I - "$MNT/big.bin" <<'PY'
import sys, hashlib
# 決定的な 3MB+α (クラスタ境界をまたぐ): xorshift
x = 88172645463325252; out = bytearray()
for _ in range((3 * 1024 * 1024 + 777) // 8 + 1):
    x ^= (x << 13) & (2**64 - 1); x ^= x >> 7; x ^= (x << 17) & (2**64 - 1)
    out += x.to_bytes(8, "little")
open(sys.argv[1], "wb").write(bytes(out[: 3 * 1024 * 1024 + 777]))
PY
: > "$MNT/empty.txt"
sync
( cd "$MNT" && find . -mindepth 1 | sort | while IFS= read -r p; do
    p=${p#./}
    if [ -d "$p" ]; then echo "d 0 - $p"
    else echo "f $(stat -c %s "$p") $(sha256sum < "$p" | cut -d' ' -f1) $p"; fi
  done ) > "$OUT/ntfs_basic.manifest"
fusermount3 -u "$MNT"; trap - EXIT; rmdir "$MNT"
echo "$IMG"; cat "$OUT/ntfs_basic.manifest"
