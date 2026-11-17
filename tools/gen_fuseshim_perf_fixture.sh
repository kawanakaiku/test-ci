#!/bin/sh
# fuseshim の性能測定用の NTFS イメージ (docs/fuse-layer.md 5 章)。mkntfs + ntfs-3g (FUSE) で作る。出力: <dir>/ntfs_perf.img
#   /big.bin        256 MiB (乱数。連続読みのスループット)
#   /frag_a.bin /frag_b.bin  各 48 MiB を 1 MiB ずつ交互に追記 (断片化したランリスト)
#   /many/f_NNNNN.txt  20000 個の小さなファイル (INDEX_ALLOCATION が大きいディレクトリの列挙)
# 使い方: gen_fuseshim_perf_fixture.sh <出力ディレクトリ>   (tmpfs か runner のディスク。約 700 MB)
set -eu
OUT=${1:?出力ディレクトリ}
MKNTFS=$(command -v mkntfs || echo /sbin/mkntfs)
{ [ -x "$MKNTFS" ] && command -v ntfs-3g >/dev/null && command -v fusermount3 >/dev/null; } || { echo "mkntfs/ntfs-3g/fusermount3 が無い" >&2; exit 1; }
mkdir -p "$OUT"; IMG=$OUT/ntfs_perf.img; MNT=$OUT/mnt.$$
rm -f "$IMG"; truncate -s 640M "$IMG"
"$MKNTFS" -F -Q -q -s 512 -c 4096 -L PERFVOL "$IMG" >/dev/null 2>&1
mkdir -p "$MNT"; ntfs-3g -o rw "$IMG" "$MNT"
trap 'fusermount3 -u "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null' EXIT
head -c 268435456 /dev/urandom > "$MNT/big.bin"
python3 -I - "$MNT" <<'PY'
import os, sys
m = sys.argv[1]
chunk = os.urandom(1 << 20)
with open(m + '/frag_a.bin', 'wb') as a, open(m + '/frag_b.bin', 'wb') as b:
    for i in range(48):
        a.write(chunk); a.flush(); b.write(chunk); b.flush()
os.mkdir(m + '/many')
for i in range(20000):
    with open('%s/many/f_%05d.txt' % (m, i), 'wb') as f:
        f.write(b'perf file %d\n' % i)
PY
fusermount3 -u "$MNT"; trap - EXIT; rmdir "$MNT"
ls -l "$IMG"
