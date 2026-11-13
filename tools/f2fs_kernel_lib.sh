# SPDX-License-Identifier: Apache-2.0
# F2FS のテスト用: 本物の Linux カーネルでイメージを読む / 操作する共通関数 (source して使う。docs/f2fs-write.md)。
# 前提: $HERE (tools の絶対パス) と $W (作業ディレクトリ) が設定済みで、$W/kmnt が作れること。
#   KMODE: loop = sudo -n で f2fs をループマウント (CI) / vm = QEMU/TCG の VM (tools/f2fs_vm/kcheck.py) / off = カーネルの検査を省く。環境変数 USBMEM_F2FS_KERNEL で固定できる。
#   ktree <img> <out> [ops]   カーネルでマウント (ops があれば書き込み可で操作を実行してから) してツリーを <out> に書く (tools/ops_ref.py --tree と同じ書式)。成功時に nker を増やす。
#   f2k_cleanup               ループマウントが残っていたら外す (trap から呼ぶ)
KMODE=${USBMEM_F2FS_KERNEL:-auto}
nker=${nker:-0}
if [ "$KMODE" = auto ]; then
  KMODE=off
  if sudo -n true 2>/dev/null && { grep -qw f2fs /proc/filesystems || sudo -n modprobe f2fs 2>/dev/null; } && grep -qw f2fs /proc/filesystems && command -v losetup >/dev/null; then KMODE=loop
  elif python3 -I "$HERE/f2fs_vm/kcheck.py" --probe >/dev/null 2>&1; then KMODE=vm; fi
fi
mkdir -p "$W/kmnt"
f2k_cleanup() { if mountpoint -q "$W/kmnt" 2>/dev/null; then sudo -n umount "$W/kmnt" 2>/dev/null || true; fi; }
ktree() {
  _img=$1; _out=$2; _ops=${3:-}
  case $KMODE in
    off) return 0 ;;
    loop)
      if [ -n "$_ops" ]; then sudo -n mount -t f2fs -o loop "$_img" "$W/kmnt" || return 1; else sudo -n mount -t f2fs -o loop,ro "$_img" "$W/kmnt" || return 1; fi
      if [ -n "$_ops" ]; then sudo -n python3 -I "$HERE/ops_ref.py" "$W/kmnt" "$_ops" || { sudo -n umount "$W/kmnt"; return 1; }; fi
      sudo -n python3 -I "$HERE/ops_ref.py" --tree "$W/kmnt" > "$_out"; sudo -n umount "$W/kmnt" ;;
    vm)
      if [ -n "$_ops" ]; then python3 -I "$HERE/f2fs_vm/kcheck.py" --ops "$_ops" "$_img" "$_out"; else python3 -I "$HERE/f2fs_vm/kcheck.py" --ro "$_img" "$_out"; fi ;;
  esac
  nker=$((nker + 1))
}
