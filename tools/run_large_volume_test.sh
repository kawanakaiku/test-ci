#!/bin/sh
# 大容量 (3 TiB の疎イメージ) の NTFS: (1) 読み取り専用マウントが $Bitmap 全体を読まずに即座に終わること (カーネルと同じく、空きクラスタ数の
# 事前計算はマウント後の非同期ワーク。互換層が同期実行していた頃は 7.3 TiB の USB ディスクで 3 分以上かかった)、
# (2) 書き込み (空きクラスタが必要な操作) では保留中の計算が実行され、正しく動くこと。
# 使い方: run_large_volume_test.sh <usbmem_ntfs_tests>
set -eu
T=$1
MKNTFS=$(command -v mkntfs || echo /sbin/mkntfs)
[ -x "$MKNTFS" ] && command -v ntfsfix >/dev/null || { echo "skip: mkntfs/ntfsfix not available" >&2; exit 77; }
W=$(mktemp -d /dev/shm/usbmem_big.XXXXXX); trap 'rm -rf "$W"' EXIT
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0
truncate -s 3T "$W/big.img" 2>/dev/null || { echo "skip: cannot create 3T sparse file here" >&2; exit 77; }
"$MKNTFS" -F -Q -q -s 512 -c 4096 -L BIGVOL "$W/big.img" >/dev/null 2>&1
: > "$W/empty.manifest"
t0=$(date +%s.%N)
"$T" --manifest "$W/big.img" "$W/empty.manifest" 2>&1 | grep -aq " 0 failures" || { echo "FAIL: read-only mount of the large volume"; exit 1; }
t1=$(date +%s.%N)
el=$(python3 -c "print(round($t1 - $t0, 3))")
echo "3 TiB 読み取り専用マウント: ${el} 秒"
python3 -c "import sys; sys.exit(0 if $t1 - $t0 < 1.5 else 1)" || { echo "FAIL: mount took ${el}s (> 1.5s): free-cluster precalc is running synchronously again?"; exit 1; }
# 書き込み (ビルドが対応している場合のみ)
# 操作なしの rw マウント → アンマウントで、メディアの内容が 1 バイトも変わらないこと (実機の RAID に rw で触れる前提条件)
: > "$W/noop.ops"
h0=$(python3 "$(dirname "$0")/sparse_hash.py" "$W/big.img")
if "$T" --ops "$W/big.img" "$W/noop.ops" 2>&1 | grep -aq "mount_rw failed: errno 30"; then echo "(書き込み無効のビルド: 書き込み部分はスキップ)"; exit 0; fi
h1=$(python3 "$(dirname "$0")/sparse_hash.py" "$W/big.img")
[ "$h0" = "$h1" ] || { echo "FAIL: no-op rw mount changed the media"; exit 1; }
echo "操作なしの rw マウントでメディア不変: ok"
# USB (模擬 BOT) 越しの rw マウントで、READ コマンドの数が先読みで抑えられること (先読みが無いと、ビットマップ全体を 4 KiB ずつ読んで数万回になる。実機 7.3 TiB では約 12 万回で 2 分 40 秒)
reads=$(USBMEM_VIA_MSC=1 "$T" --ops "$W/big.img" "$W/noop.ops" 2>&1 | grep -a "^msc ops:" | grep -ao "8[8]x[0-9]*\|28x[0-9]*" | head -1 | sed 's/^[0-9a-f]*x//')
[ -n "$reads" ] || { echo "FAIL: no READ count from msc ops"; exit 1; }
echo "3 TiB rw マウントの READ コマンド数 (USB 越し): $reads"
[ "$reads" -lt 4000 ] || { echo "FAIL: too many READ commands ($reads): read-ahead not effective?"; exit 1; }
# メモリ使用量の上限: 実機の 7.3 TiB (クラスタ 2048B) で RSS が約 1.25 GB になった (ページキャッシュ無制限 + 確保の 2 倍使い)。
# サニタイザなしのビルドだけで検査する (ASan は検査用の追加メモリで数百 MB になる)。キャッシュ上限 + 小さな固定メモリで収まること
if ! ldd "$T" 2>/dev/null | grep -q asan && [ -x /usr/bin/time ]; then
  truncate -s 7500G "$W/huge.img" && "$MKNTFS" -F -Q -q -s 512 -c 2048 -L HUGE "$W/huge.img" >/dev/null 2>&1
  out=$(USBMEM_VIA_MSC=1 /usr/bin/time -f "%M %e" "$T" --ops "$W/huge.img" "$W/noop.ops" 2>&1 >/dev/null | tail -1)
  rss=${out% *}; secs=${out#* }
  echo "7.3 TiB (クラスタ 2048B) rw マウントの最大 RSS: ${rss} KB, 所要 ${secs} 秒"
  # 空きクラスタ数の計算 (ビットマップ約 500 MB) が CPU 律速になっていないこと (ビット単位だと PC で 4 秒、実機 Xperia で約 40 秒だった。ワード単位で約 1 秒)
  awk -v s="$secs" 'BEGIN { exit !(s < 3.0) }' || { echo "FAIL: rw mount took ${secs}s (> 3s): bitmap scan too slow?"; exit 1; }
  [ "$rss" -lt 150000 ] || { echo "FAIL: RSS ${rss} KB (> 150 MB): page cache not bounded?"; exit 1; }
fi
printf 'mkdir /d\ncreate /d/f.txt\nwrite /d/f.txt 0 20000 5\n' > "$W/ops"
out=$("$T" --ops "$W/big.img" "$W/ops" 2>&1 | grep -av unimplemented || true)
if echo "$out" | grep -aq "mount_rw failed: errno 30"; then echo "(書き込み無効のビルド: 書き込み部分はスキップ)"; exit 0; fi
echo "$out" | grep -aq "ops: 3 run, 0 failed" || { echo "FAIL: write on large volume: $out" | cut -c1-200; exit 1; }
ntfsfix -n "$W/big.img" >/dev/null 2>&1 || { echo "FAIL: ntfsfix after write"; exit 1; }
echo "large volume tests ok"
