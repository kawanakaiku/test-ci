#!/bin/sh
# 自前ドライバが書いた NTFS イメージを、Windows 自身 (WinPE の chkdsk / certutil) に検証させる。
# 使い方: verify_with_windows.sh <Win11.iso> <image (MBR+NTFS, 512B)> [期待 manifest]  → ログを標準出力へ。
# WinPE には fc/comp/certutil が無いので、Windows にボリューム全体をログディスク (L:\ のルート) へコピーさせ、
# ホスト側で manifest (期待 sha256) と照合する = 「Windows が読んだ内容」が期待どおりかを確認する。期待 manifest は第 3 引数。
# chkdsk は修復なし (/f なし) で実行するので、イメージは変更されない。
set -eu
ISO=${1:?usage: $0 <Win11.iso> <image>}; IMG=$2
HERE=$(cd "$(dirname "$0")" && pwd)
T=${USBMEM_TESTS_BIN:-$HERE/../../build/host/wsan/compat/usbmem_ntfs_tests}
W=$(mktemp -d /dev/shm/usbmem_ver.XXXXXX); trap 'rm -rf "$W"' EXIT
VARIANT=verify; [ -z "${USBMEM_VERIFY_FIX:-}" ] || VARIANT=verifyfix
python3 -I "$HERE/make_payload.py" "$W" $VARIANT >/dev/null
cp --reflink=auto "$IMG" "$W/target.raw"
truncate -s ${USBMEM_LOG_DISK:-256M} "$W/usblog.raw"
python3 -I "$HERE/run_vm.py" "$W" "$ISO" 600 >&2
# USBMEM_KEEP_TARGET=<path>: 修復後のイメージを保存 (ホストで自前ドライバに読み直させる用)
[ -z "${USBMEM_KEEP_TARGET:-}" ] || cp "$W/target.raw" "$USBMEM_KEEP_TARGET"
# ログディスクは Windows が NTFS で作る → 自前ドライバで読む (ここでも Windows 製 NTFS の読み取り検証になる)
ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 "$T" --cat "$W/usblog.raw" verify.log 2>/dev/null | iconv -f CP932 -t UTF-8 -c | tr -d '\r'

if [ -n "${3:-}" ]; then
  cp "$3" "$W/m.prefixed"
  ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 "$T" --manifest "$W/usblog.raw" "$W/m.prefixed" 2>&1 | grep -E "manifest:|FAIL" | sed 's/^/WINDOWS-READ: /'
fi
