#!/bin/sh
# qcow2 + nbdfuse で「見かけ 1TiB 級・実使用は数十 MB」の NTFS を tmpfs 上に作り、読み取りテストと fuzz を回す。
# 使い方: ntfs_large_test.sh <usbmem_ntfs_tests のパス> [仮想サイズ(既定 1T)]
# 大きなボリューム特有の経路 (64bit LCN、MFT ゾーン後の高位クラスタ、巨大な $Bitmap) を、メモリをほぼ使わずに検証する。
set -eu
TESTS=$1; SIZE=${2:-1T}
for t in qemu-img qemu-nbd nbdfuse mkntfs ntfs-3g fusermount3; do command -v $t >/dev/null || [ -x /sbin/$t ] || { echo "skip: $t not available" >&2; exit 77; }; done
D=/dev/shm/usbmem_ntfs_large_$USER; rm -rf "$D"; mkdir -p "$D"
QCOW=$D/disk.qcow2
qemu-img create -q -f qcow2 "$QCOW" "$SIZE"
: > "$D/ntfs_basic.img"
# detect-zeroes: mkntfs が書くゼロ領域を実体化させない
nbdfuse -o allow_other "$D/ntfs_basic.img" [ qemu-nbd -f qcow2 --discard=unmap --detect-zeroes=unmap "$QCOW" ] &
NBD=$!
cleanup() { fusermount3 -u "$D/ntfs_basic.img" 2>/dev/null || true; kill $NBD 2>/dev/null || true; rm -rf "$D"; }
trap cleanup EXIT
i=0; while [ ! -s "$D/ntfs_basic.img" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done
[ "$(stat -c %s "$D/ntfs_basic.img")" -gt 1000000000 ] || { echo "nbdfuse did not expose the disk" >&2; exit 1; }
# 1TiB でも 512B セクタ/4KiB クラスタで作る。gen_ntfs_fixture.sh が同じ内容 (圧縮・スパース・800 件ディレクトリ等) を書く
USBMEM_NTFS_EXISTING_IMG=1 "$(dirname "$0")/gen_ntfs_fixture.sh" "$D" >/dev/null
echo "qcow2 実使用: $(du -h "$QCOW" | cut -f1) / 仮想: $SIZE / ボリュームサイズ: $(stat -c %s "$D/ntfs_basic.img") bytes"
"$TESTS" "$D"
"$TESTS" --fuzz "$D" 300 91
