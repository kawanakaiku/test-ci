#!/bin/sh
# 4 GiB を超えるオフセットにある暗号パーティション (GPT + LUKS2 / BitLocker) の検証。docs/crypto-volumes.md 6.5
# 大きな疎ディスクは nbdfuse で作る (tools/ci/nbdfuse.sh)。ファイルシステムには何も書かない。ルート権限不要。
# 使い方: run_cryptodisk_highoffset_test.sh <usbmem_cryptodisk_tests> [出力ディレクトリ]
set -u
T=$1
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH="$PATH:/sbin:/usr/sbin:$HOME/.local/bin"
for c in nbdfuse nbdkit fusermount3; do
  command -v "$c" >/dev/null 2>&1 || { echo "skip: $c not available" >&2; exit 77; }
done
. "$HERE/ci/nbdfuse.sh"
OWN_OUT=; [ -n "${2:-}" ] || OWN_OUT=1
OUT=${2:-${NBDFUSE_ROOT}/cryptodisk_high_out_$$}
DIR=$NBDFUSE_ROOT/cryptodisk_high_$$
cleanup() { nbdfuse_stop "$DIR"; [ -n "$OWN_OUT" ] && rm -rf "$OUT"; }
trap cleanup EXIT
rm -rf "$OUT"; mkdir -p "$OUT"
nbdfuse_start "$DIR" 8G || exit 1
python3 -I "$HERE/gen_cryptodisk_fixtures.py" --high-offset "$DIR/nbd" "$OUT" >&2 || { echo "FAIL: fixture generation failed" >&2; exit 1; }
sh "$HERE/run_cryptodisk_tests.sh" "$T" high "$OUT"
