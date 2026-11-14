#!/bin/bash
# nbdfuse の疎イメージが期待どおり動くかの確認 (tools/ci/nbdfuse.sh)。大きなサイズ、遠いオフセットの読み書き、メモリ使用量、mkfs。
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/nbdfuse.sh"
export PATH="$PATH:/sbin:/usr/sbin"
SIZE=${1:-40G}
D=$NBDFUSE_ROOT/smoke.$$
trap 'nbdfuse_stop "$D"' EXIT
nbdfuse_start "$D" "$SIZE"
img=$D/nbd
sz=$(stat -c %s "$img"); echo "size=$sz"
[ "$sz" -ge $((39 << 30)) ] || { echo "サイズが想定より小さい"; exit 1; }
# 遠いオフセットへ書いて、読み戻す (先頭と 39 GiB 付近)
printf 'HEAD-MARKER' | dd of="$img" bs=1 seek=0 conv=notrunc status=none
printf 'FAR-MARKER-39G' | dd of="$img" bs=1 seek=$((39 << 30)) conv=notrunc status=none
[ "$(dd if="$img" bs=1 skip=0 count=11 status=none)" = "HEAD-MARKER" ]
[ "$(dd if="$img" bs=1 skip=$((39 << 30)) count=14 status=none)" = "FAR-MARKER-39G" ]
# 触っていない所はゼロ
[ "$(dd if="$img" bs=1M skip=20000 count=4 status=none | tr -d '\0' | wc -c)" = 0 ]
echo "遠いオフセットの読み書き: OK"
# mkfs (疎のまま)。XFS は大きなイメージだと AG が複数になり、あちこちに書く
if command -v mkfs.xfs >/dev/null; then mkfs.xfs -q -f "$img"; echo "mkfs.xfs: OK"; fi
if command -v xfs_db >/dev/null; then xfs_db -r -c 'sb 0' -c 'print agcount dblocks' "$img"; fi
# メモリ使用量: nbdkit の常駐メモリ (触れたページ分だけのはず)
pid=$(pgrep -f "nbdkit -s memory $SIZE" | head -1 || true)
if [ -n "$pid" ]; then
  rss=$(awk '/^VmRSS/{print $2}' /proc/$pid/status)
  echo "nbdkit RSS = $((rss / 1024)) MiB (イメージ ${SIZE})"
  [ "$rss" -lt $((2 * 1024 * 1024)) ] || { echo "RSS が 2 GiB を超えた (疎でない?)"; exit 1; }
fi
echo "nbdfuse smoke: OK"
