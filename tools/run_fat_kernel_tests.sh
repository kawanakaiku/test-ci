#!/bin/sh
# FAT12/16/32 書き込みのカーネル (Linux vfat) との突き合わせ (CI 専用: root か sudo -n と loop マウントが要る。ローカルでは使わない)。docs/fat-write.md
#   A. 自前ドライバが書いたイメージ → カーネルの vfat が (fsck なしで、そのまま) 読めて、ホストの POSIX 操作で作った期待ツリーと一致すること。
#      カーネルが "Volume was not properly unmounted" を出さない (ダーティ印が残っていない) こと。さらにカーネルが rw でマウントして 1 ファイル書き、fsck.fat -n が clean なこと。
#   B. カーネルの vfat が (rw マウントで) 同じ操作列を実行したイメージ → 自前ドライバが読んで (--tree)、期待ツリーと一致すること (カーネルが書いた長い名前・8.3 名・時刻・属性を読めること)。
# 使い方: run_fat_kernel_tests.sh <usbmem_fat_write_tests> [フィクスチャ dir (tools/gen_fat_fixture.py)]   終了コード 77 = 前提が無い (スキップ)
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_fat_${USER:-u}}
HERE=$(cd "$(dirname "$0")" && pwd)
for c in fsck.fat 7z python3 mount umount; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
if [ "$(id -u)" = 0 ]; then SU=""; else SU="sudo -n"; $SU true 2>/dev/null || { echo "skip: needs root or passwordless sudo" >&2; exit 77; }; fi
CASES="fat12_c4k:4096:1500000:150 fat16_c512:512:3000000:200 fat32_c4k:4096:3000000:300"
for c in $CASES; do n=${c%%:*}; [ -f "$D/$n.img" ] || { echo "skip: no fixture $D/$n.img" >&2; exit 77; }; done
W=$(mktemp -d "${USBMEM_TMP:-/dev/shm}"/usbmem_fkt.XXXXXX)
mkdir "$W/mnt"
cleanup() { $SU umount "$W/mnt" 2>/dev/null || true; $SU rm -rf "$W"; }
trap cleanup EXIT
export LC_ALL=C.UTF-8 TZ=UTC
rc=0
fail() { echo "FAIL: $*"; rc=1; }
kmount() {  # kmount <img> <ro|rw>
  $SU mount -t vfat -o "loop,$2,iocharset=utf8" "$1" "$W/mnt"
}
tree7z() {  # tree7z <img> <out>
  rm -rf "$W/x7"; mkdir "$W/x7"; 7z x -y -o"$W/x7" "$1" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/x7" > "$2"
}
dmesg_bad() { $SU dmesg 2>/dev/null | tail -200 | grep -i -c "not properly unmounted\|FAT-fs.*error\|FAT-fs.*corrupt" || true; }
for c in $CASES; do
  IFS=: read name cl big nmany <<EOF
$c
EOF
  python3 -I "$HERE/gen_fat_ops.py" $cl "$W/ops.txt" $nmany $big
  # 期待ツリー: 元イメージを 7z で展開し、同じ操作列をホストのディレクトリに POSIX 操作で実行したもの
  tree7z "$D/$name.img" "$W/t0" || { fail "7z extract of the original ($name)"; continue; }
  rm -rf "$W/ref"; cp -r "$W/x7" "$W/ref"
  python3 -I "$HERE/ops_ref.py" "$W/ref" "$W/ops.txt" || { fail "reference ops ($name)"; continue; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/ref" > "$W/tr"

  # ---- A. 自前ドライバが書く → カーネルが読む (fsck なし)
  cp "$D/$name.img" "$W/a.img"
  "$T" --ops "$W/a.img" "$W/ops.txt" > "$W/ops.log" 2>&1 || { tail -5 "$W/ops.log"; fail "A $name: driver ops"; continue; }
  $SU dmesg -C 2>/dev/null || true
  if ! kmount "$W/a.img" ro; then fail "A $name: kernel vfat could not mount the image the driver wrote"; continue; fi
  $SU python3 -I "$HERE/ops_ref.py" --tree "$W/mnt" > "$W/ta"; $SU umount "$W/mnt"
  if cmp -s "$W/ta" "$W/tr"; then echo "[A $name] kernel vfat reads the driver's image: $(wc -l < "$W/ta") entries identical to the host reference"
  else diff "$W/ta" "$W/tr" | head -10; fail "A $name: kernel tree differs from the reference"; continue; fi
  [ "$(dmesg_bad)" = 0 ] || { $SU dmesg | tail -20; fail "A $name: kernel reported an unclean/corrupt volume"; continue; }
  # カーネルが続けて書く: rw マウント → 1 ファイル追加 → umount → fsck.fat clean
  kmount "$W/a.img" rw || { fail "A $name: kernel rw mount"; continue; }
  $SU sh -c "echo kernel-after-driver > '$W/mnt/kernel_added.txt'"; $SU umount "$W/mnt"
  fsck.fat -n "$W/a.img" > "$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; fail "A $name: fsck.fat not clean after the kernel wrote on top of the driver's image"; continue; }

  # ---- B. カーネルが書く → 自前ドライバが読む
  cp "$D/$name.img" "$W/b.img"
  kmount "$W/b.img" rw || { fail "B $name: kernel rw mount"; continue; }
  if ! $SU python3 -I "$HERE/ops_ref.py" "$W/mnt" "$W/ops.txt"; then $SU umount "$W/mnt"; fail "B $name: ops on the kernel mount"; continue; fi
  $SU umount "$W/mnt"
  "$T" --tree "$W/b.img" "$W/tb" || { fail "B $name: driver could not read the kernel's image"; continue; }
  if cmp -s "$W/tb" "$W/tr"; then echo "[B $name] driver reads the kernel's image: $(wc -l < "$W/tb") entries identical to the host reference"
  else diff "$W/tb" "$W/tr" | head -10; fail "B $name: driver tree differs from the reference"; fi
  # カーネルが書いたイメージに自前ドライバが続けて書く: 何もせず rw マウント → unmount でイメージ不変、1 ファイル作成後 fsck.fat clean
  cp "$W/b.img" "$W/b0.img"; "$T" --noop "$W/b.img" > /dev/null 2>&1 || fail "B $name: noop on the kernel's image"
  cmp -s "$W/b.img" "$W/b0.img" || fail "B $name: a no-op mount changed the kernel's image"
  printf 'create /driver_added.txt\nwrite /driver_added.txt 0 100 3\n' > "$W/ops2.txt"
  "$T" --ops "$W/b.img" "$W/ops2.txt" > "$W/ops2.log" 2>&1 || { tail -5 "$W/ops2.log"; fail "B $name: driver ops on the kernel's image"; continue; }
  fsck.fat -n "$W/b.img" > "$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; fail "B $name: fsck.fat not clean after the driver wrote on top of the kernel's image"; }
done
[ $rc = 0 ] && echo "fat kernel tests: OK"
exit $rc
