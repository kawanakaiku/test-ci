#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# kfsd の性能の測定 (docs/kfsd.md 4 章)。ext4 / exFAT / FAT32 / NTFS の空のイメージ (tmpfs) を作り、usbmem_kfsd_bench でプロセス内と RPC を比べる。
#   使い方: tools/run_kfsd_bench.sh <usbmem_kfsd_bench> <kfsd> [FS...]   (既定: ext4 exfat fat32 ntfs)
# 道具が無い FS は飛ばす。1 つも測れなければ 77 (SKIP)。
set -u
bench=${1:?usbmem_kfsd_bench}; kfsd=${2:?kfsd}; shift 2
fss=${*:-"ext4 exfat fat32 ntfs"}
export PATH="$PATH:/sbin:/usr/sbin"
base=/dev/shm; [ -w "$base" ] || base=/tmp
W=$(mktemp -d "$base/usbmem_kfsd_benchXXXXXX"); trap 'rm -rf "$W"' EXIT
n=0
for fs in $fss; do
  img=$W/$fs.img; rm -f "$img"; truncate -s 640M "$img"
  case $fs in
    ext4)  command -v mke2fs >/dev/null && mke2fs -q -t ext4 -b 4096 "$img" || continue ;;
    ext2)  command -v mke2fs >/dev/null && mke2fs -q -t ext2 -b 4096 "$img" || continue ;;
    exfat) command -v mkfs.exfat >/dev/null && mkfs.exfat -q -c 32768 "$img" >/dev/null || continue ;;
    fat32) command -v mkfs.fat >/dev/null && mkfs.fat -F 32 -s 8 -S 512 -R 32 "$img" >/dev/null 2>&1 || continue ;;
    ntfs)  command -v mkntfs >/dev/null && mkntfs -F -Q -q "$img" >/dev/null 2>&1 || continue ;;
    *) continue ;;
  esac
  echo "== $fs" >&2
  "$bench" --kfsd "$kfsd" "$img" --size-mb 128 --files 3000 || { rc=$?; [ $rc -eq 77 ] && { rm -f "$img"; continue; }; echo "FAIL: bench $fs (rc=$rc)" >&2; exit 1; }
  n=$((n+1)); rm -f "$img"
done
[ $n -gt 0 ] || exit 77
