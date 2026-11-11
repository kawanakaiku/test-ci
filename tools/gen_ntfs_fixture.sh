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
# USBMEM_NTFS_EXISTING_IMG=1: 既に用意されたデバイス/ファイル (nbdfuse 経由の大容量仮想ディスク等) をそのまま使う
if [ "${USBMEM_NTFS_EXISTING_IMG:-0}" != 1 ]; then rm -f "$IMG"; truncate -s 64M "$IMG"; fi
"$MKNTFS" -F -Q -q -s 512 -c "${USBMEM_NTFS_CLUSTER:-4096}" -L TESTVOL "$IMG" >/dev/null 2>&1
mkdir -p "$MNT"; ntfs-3g -o rw,compression "$IMG" "$MNT"
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
# 800 ファイルのディレクトリ: $INDEX_ALLOCATION (B+ツリー) を使わせる
mkdir "$MNT/many"
i=1; while [ $i -le 800 ]; do printf 'file number %d\n' $i > "$MNT/many/file_$i.txt"; i=$((i+1)); done
# スパースファイル 20MiB (末尾 4 バイトだけ実データ)
truncate -s 20M "$MNT/sparse.bin"
printf 'tail' | dd of="$MNT/sparse.bin" bs=1 seek=$((20*1024*1024-4)) conv=notrunc 2>/dev/null
# 圧縮ディレクトリ (LZNT1): 属性 0x800 を付けたディレクトリ配下の新規ファイルは圧縮される
mkdir "$MNT/cdir"
python3 -I -c "import os; os.setxattr('$MNT/cdir','system.ntfs_attrib_be',bytes.fromhex('00000830'))"
python3 -I - "$MNT/cdir" <<'PYEOF'
import os, sys, random
d = sys.argv[1]
open(d + '/text.txt', 'wb').write(b''.join(b'compressible text line %d\n' % i for i in range(20000)))
r = random.Random(7)
open(d + '/random.bin', 'wb').write(bytes(r.getrandbits(8) for _ in range(300000)))      # 非圧縮性 (圧縮ユニットが生のまま残る)
mix = bytearray()
for k in range(40):                                                                         # 圧縮可 / 不可ブロックの混在 + 全ゼロ(スパース)ブロック
    mix += (b'abc' * 5000) if k % 3 == 0 else (bytes(r.getrandbits(8) for _ in range(9000)) if k % 3 == 1 else bytes(20000))
open(d + '/mixed.bin', 'wb').write(bytes(mix))
PYEOF
: > "$MNT/empty.txt"
sync
( cd "$MNT" && find . -mindepth 1 | sort | while IFS= read -r p; do
    p=${p#./}
    if [ -d "$p" ]; then echo "d 0 - $p"
    else echo "f $(stat -c %s "$p") $(sha256sum < "$p" | cut -d' ' -f1) $p"; fi
  done ) > "$OUT/ntfs_basic.manifest"
fusermount3 -u "$MNT"; trap - EXIT; rmdir "$MNT"
echo "$IMG"; cat "$OUT/ntfs_basic.manifest"
