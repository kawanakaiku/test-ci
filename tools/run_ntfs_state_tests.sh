#!/bin/sh
# NTFS の「ダーティ / 未再生ログ / 休止状態」のボリュームの扱い (docs/dirty-volumes.md)。
# tools/gen_ntfs_state_fixture.py が作る 5 つの 16 MiB イメージに対して usbmem_ntfs_tests --state を実行する (state() の値、rw を要求しても止まること、止めたイメージが 1 バイトも変わらないこと)。
# 使い方: run_ntfs_state_tests.sh <usbmem_ntfs_tests> [作業ディレクトリ (tmpfs。既定 /dev/shm/usbmem_ntfs_state_$USER)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_ntfs_state_${USER:-u}}
for c in mkntfs ntfs-3g fusermount3 python3; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D"
trap 'rm -rf "$D"' EXIT
rc=0
python3 -I "$HERE/gen_ntfs_state_fixture.py" "$D" || rc=$?
[ "$rc" -eq 0 ] || exit "$rc"   # 77 (ツールなし) もそのまま返す
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
"$T" --state "$D"
