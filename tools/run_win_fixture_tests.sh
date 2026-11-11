#!/bin/sh
# Windows (WinPE) が作った NTFS イメージの検証。イメージが無ければ SKIP (終了コード 77)。
# イメージは tools/winpe/make_windows_fixtures.sh <Win11.iso> で作る (Windows ISO は同梱・配布しない)。
# 使い方: run_win_fixture_tests.sh <usbmem_ntfs_tests> [イメージディレクトリ]
set -eu
T=$1; D=${2:-/dev/shm/usbmem_winntfs_${USER:-u}}
[ -f "$D/win_mbr.img" ] && [ -f "$D/manifest.txt" ] || { echo "skip: Windows fixtures not found in $D (make with tools/winpe/make_windows_fixtures.sh)" >&2; exit 77; }
rc=0
for im in win_mbr win_gpt64k win_dirty; do
  echo "== $im"; "$T" --manifest "$D/$im.img" "$D/manifest.txt" || rc=1
  "$T" --fuzz "$D" 200 7 "$im.img" || rc=1
done
echo "== win_usb4kn (論理ブロック 4096)"
USBMEM_IMG_BS=4096 "$T" --manifest "$D/win_usb4kn.img" "$D/manifest.txt" || rc=1
USBMEM_IMG_BS=4096 "$T" --fuzz "$D" 200 7 win_usb4kn.img || rc=1
exit $rc
