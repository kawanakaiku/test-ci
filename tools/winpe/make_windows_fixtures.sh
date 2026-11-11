#!/bin/sh
# 実際の Windows (Win11 インストール ISO の WinPE) に NTFS を作らせ、フィクスチャイメージを出力する。
# 使い方: make_windows_fixtures.sh <Win11.iso> [出力ディレクトリ(既定 /dev/shm/usbmem_winntfs_$USER)]
# 必要: qemu-system-x86_64 (KVM)、genisoimage、python3。所要 ≈ 2 分 × 3 バリアント。画像は tmpfs (/dev/shm) に作る。
# 出力: win_mbr.img win_gpt64k.img (IDE 2 本) / win_usb4kn.img (USB 4Kn) / win_dirty.img (ダーティフラグ付き) と manifest.txt
set -eu
ISO=${1:?usage: $0 <Win11.iso> [outdir]}
OUT=${2:-/dev/shm/usbmem_winntfs_$USER}
HERE=$(cd "$(dirname "$0")" && pwd)
{ command -v qemu-system-x86_64 >/dev/null && command -v genisoimage >/dev/null && [ -r /dev/kvm ] && [ -f "$ISO" ]; } || { echo "skip: qemu/kvm/genisoimage/iso not available" >&2; exit 77; }
mkdir -p "$OUT"
for V in mbr_gpt usb4kn dirty; do
  W=$OUT/work_$V; rm -rf "$W"; mkdir -p "$W"
  python3 -I "$HERE/make_payload.py" "$W" $V
  case $V in
    mbr_gpt) truncate -s 256M "$W/target.raw"; truncate -s 256M "$W/target2.raw" ;;
    usb4kn)  truncate -s 256M "$W/usb4k.raw" ;;
    dirty)   truncate -s 256M "$W/target.raw" ;;
  esac
  python3 -I "$HERE/run_vm.py" "$W" "$ISO" 900
  case $V in
    mbr_gpt) mv "$W/target.raw" "$OUT/win_mbr.img"; mv "$W/target2.raw" "$OUT/win_gpt64k.img" ;;
    usb4kn)  mv "$W/usb4k.raw" "$OUT/win_usb4kn.img" ;;
    dirty)   mv "$W/target.raw" "$OUT/win_dirty.img" ;;
  esac
  cp "$W/manifest.txt" "$OUT/manifest.txt"
  rm -rf "$W"
done
ls -la "$OUT"
