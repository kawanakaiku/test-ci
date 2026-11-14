#!/bin/bash
# usbmem_fuse のスモークテスト (root で実行)。イメージを mkfs → マウント → 基本操作 → アンマウント → fsck。
#   使い方: smoke.sh FS [usbmem_fuse のパス]     FS = ext4 | ext2 | fat | exfat | ntfs | xfs | btrfs | f2fs
set -u
export PATH="$PATH:/sbin:/usr/sbin"
FS=${1:?fs}; BIN=${2:-usbmem_fuse}
W=$(mktemp -d /mnt/usbmem_smoke.XXXXXX 2>/dev/null || mktemp -d)
IMG=$W/img; MNT=$W/mnt; mkdir -p "$MNT"; truncate -s 512M "$IMG"
case $FS in
  ext4) mkfs.ext4 -q -F -b 4096 "$IMG"; FSCK='e2fsck -fn' ;;
  ext2) mkfs.ext2 -q -F -b 4096 "$IMG"; FSCK='e2fsck -fn' ;;
  fat) mkfs.fat -F 32 -s 8 "$IMG" >/dev/null; FSCK='fsck.fat -n' ;;
  exfat) mkfs.exfat "$IMG" >/dev/null; FSCK='fsck.exfat -n' ;;
  ntfs) mkntfs -F -Q -q "$IMG" >/dev/null; FSCK=true ;;
  xfs) mkfs.xfs -q -f -m rmapbt=0 -n parent=0 "$IMG"; FSCK='xfs_repair -n' ;;
  btrfs) mkfs.btrfs -q -f "$IMG" >/dev/null; FSCK='btrfs check --readonly' ;;
  f2fs) mkfs.f2fs -q -f "$IMG" >/dev/null; FSCK='fsck.f2fs --dry-run -f' ;;
  *) echo "unknown fs $FS"; exit 2 ;;
esac
fail=0
chk() { if ! "$@"; then echo "SMOKE-FAIL: $*"; fail=1; fi; }
export USBMEM_FUSE_LOG=$W/fuse.log USBMEM_FUSE_TRACE=${USBMEM_FUSE_TRACE:-1}
"$BIN" -o allow_other,default_permissions "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
grep -w "$MNT" /proc/mounts
chk mkdir "$MNT/d"
chk sh -c "echo hello > $MNT/d/a"
chk test "$(cat "$MNT/d/a")" = hello
chk sh -c "head -c 3000000 /dev/urandom > $MNT/big"
chk sh -c "cp $MNT/big $MNT/d/big2 && cmp $MNT/big $MNT/d/big2"
chk mv "$MNT/d/a" "$MNT/d/b"
chk ln -s b "$MNT/d/sym"
chk test "$(readlink "$MNT/d/sym")" = b
chk ln "$MNT/d/b" "$MNT/d/hard"
chk truncate -s 1000 "$MNT/big"
chk test "$(stat -c %s "$MNT/big")" = 1000
chk rm "$MNT/d/b"
ls -la "$MNT" "$MNT/d"
df "$MNT" | cat
umount "$MNT" || { fusermount3 -u "$MNT"; }
sleep 1
case $FS in ext*) dumpe2fs -h "$IMG" 2>&1 | grep -i "features\|state\|journal\|orphan\|free" ;; esac
$FSCK "$IMG"; rc=$?
echo "fsck rc=$rc"
[ $rc -eq 0 ] || { echo "SMOKE-FAIL: fsck $FS rc=$rc"; fail=1; }
if [ $fail -ne 0 ] || grep -q "error" "$W/fuse.log"; then echo "--- usbmem_fuse log (head) ---"; head -5 "$W/fuse.log"; echo ...; tail -60 "$W/fuse.log"; fi
rm -rf "$W"
exit $fail
