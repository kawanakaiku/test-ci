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
echo "== 模擬 USB Mass Storage 経由 (4Kn は 4096B ブロック)"
USBMEM_VIA_MSC=1 "$T" --manifest "$D/win_mbr.img" "$D/manifest.txt" || rc=1
USBMEM_VIA_MSC=1 USBMEM_IMG_BS=4096 "$T" --manifest "$D/win_usb4kn.img" "$D/manifest.txt" || rc=1
# 暗号化イメージ (tools/winvhd/make_encrypted_fixtures.sh で作る。無ければ省略)
if [ -f "$D/win_efs.img" ]; then
  echo "== win_efs: 平文は読め、EFS 暗号化ファイルは -EOPNOTSUPP(95) で安全に拒否 (クラッシュしない)"
  "$T" --cat "$D/win_efs.img" plain.txt 2>/dev/null | grep -q "plain file" || { echo "FAIL efs plain"; rc=1; }
  for p in secret_single.txt secret/a.txt secret/random.bin; do
    "$T" --cat "$D/win_efs.img" "$p" 2>&1 >/dev/null | grep -aq "read error -95" || { echo "FAIL efs $p not refused"; rc=1; }
  done
  "$T" --fuzz "$D" 200 7 win_efs.img || rc=1
fi
if [ -f "$D/win_bitlocker.img" ]; then
  echo "== win_bitlocker: BitLocker と判別され、NTFS としてはマウントされない"
  "$T" --probe "$D/win_bitlocker.img" | grep -q "bitlocker" || { echo "FAIL bitlocker not detected"; rc=1; }
fi
exit $rc
