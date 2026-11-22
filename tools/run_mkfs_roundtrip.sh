#!/bin/sh
# 自前 mkfs で作った FS をアプリのドライバで読み書きし、外部の fsck で確かめる (docs/mkfs-extended.md 7 章)。
# 使い方: run_mkfs_roundtrip.sh <usbmem_mkfs_roundtrip>
# 書き込みビルド (USBMEM_NTFS_WRITE=ON) でなければ、ドライバが rw を拒否して終了コード 77 (SKIP) になる。
# 外部ツールで見る: ext3/4 = e2fsck -fn (終了 0 かつ WARNING なし)、FAT = fsck.fat -n、exFAT = fsck.exfat -n、NTFS = ntfsfix -n。
# ジャーナル無しの ext4 はドライバが読み取り専用にするので往復には含めない (docs/mkfs-extended.md)。
# イメージは tmpfs に作り、終わったら消す。
set -u
export PATH="$PATH:/sbin:/usr/sbin"
T=$1
for c in e2fsck fsck.fat ntfsfix; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
W=$(mktemp -d /dev/shm/usbmem_mkrt.XXXXXX)
trap 'rm -rf "$W"' EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
rc=0
skipped=0
n=0
# run <fs> <size> [mkfs 引数...]
run() {
  fs=$1; sz=$2; shift 2
  n=$((n + 1))
  img=$W/t.img
  rm -f "$img"; truncate -s "$sz" "$img"
  out=$("$T" "$fs" "$img" "$@" 2>&1); r=$?
  if [ $r -eq 77 ]; then skipped=$((skipped + 1)); echo "skip: $fs $sz $* (no write support)"; return; fi
  if [ $r -ne 0 ]; then echo "$out" | tail -8; echo "FAIL: $fs $sz $*: round trip rc=$r"; rc=1; return; fi
  case $fs in
    ext3|ext4|ext2) o=$(e2fsck -fn "$img" 2>&1); r2=$?; echo "$o" | grep -q WARNING && r2=1 ;;
    fat12|fat16|fat32) o=$(fsck.fat -n "$img" 2>&1); r2=$? ;;
    exfat) if command -v fsck.exfat >/dev/null; then o=$(fsck.exfat -n "$img" 2>&1); r2=$?; else o=; r2=0; fi ;;
    ntfs) o=$(ntfsfix -n "$img" 2>&1); r2=$?; echo "$o" | grep -q successfully || r2=1 ;;
  esac
  if [ $r2 -ne 0 ]; then echo "$o" | tail -12; echo "FAIL: $fs $sz $*: external check after the round trip"; rc=1; return; fi
  echo "ok: $fs $sz $*"
}
run ext4 64M
run ext4 300M
run ext4 40M --cluster 1024 --label rt
run ext3 64M
run ext3 40M --cluster 2048
run ext2 32M
run fat16 64M --label RT16
run fat16 300M --cluster 8192
run fat12 8M
run fat12 1440K
run fat32 100M
run exfat 100M
run ntfs 64M
run ntfs 100M --cluster 1024 --label rt
run ntfs 300M --cluster 65536
run ntfs 100M --sector 4096
[ $skipped -eq $n ] && exit 77
exit $rc
