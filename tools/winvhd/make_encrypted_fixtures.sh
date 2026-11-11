#!/bin/sh
# 実 Windows 11 (w11 ツール) で EFS / BitLocker のイメージを作る。
# 使い方: make_encrypted_fixtures.sh [w11 のパス(既定 /run/media/u/Data/win11/w11)] [出力 /dev/shm/usbmem_winntfs_$USER]
# 前提: w11 のベースイメージが images/ にある。ゲストは --ephemeral で起動し、終わったら停止して破棄する。
# 出力: win_efs.img (EFS 暗号化ファイル入り NTFS) / win_bitlocker.img (BitLocker XTS-AES-128, パスワード保護)
set -eu
W11=${1:-/run/media/u/Data/win11/w11}; OUT=${2:-/dev/shm/usbmem_winntfs_${USER:-u}}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -x "$W11" ] || { echo "skip: w11 not found" >&2; exit 77; }
mkdir -p "$OUT"; N=ufx$$
export VM_MEM=${VM_MEM:-4G} VM_DISPLAY=none
( setsid "$W11" run $N --ephemeral >/dev/null 2>&1 & )
for i in $(seq 1 60); do "$W11" ps 2>/dev/null | grep "$N" | grep -q "no-lease" || { "$W11" ps 2>/dev/null | grep -q "$N.*192\." && break; }; sleep 10; done
sleep 10
"$W11" ssh $N --no-pty 'whoami' >/dev/null 2>&1 || { echo "guest not reachable" >&2; "$W11" ssh $N --no-pty "shutdown /s /t 0" >/dev/null 2>&1 || true; exit 1; }
"$W11" cp "$HERE/guest_efs.ps1" $N:C:/Users/user/guest_efs.ps1 >/dev/null 2>&1
"$W11" cp "$HERE/guest_bitlocker.ps1" $N:C:/Users/user/guest_bitlocker.ps1 >/dev/null 2>&1
mk() {  # mk <name> <letter> <script> [desk]
  truncate -s 256M "$OUT/$1.img"; "$W11" attach $N "$OUT/$1.img" --disk >/dev/null 2>&1; sleep 4
  d=$("$W11" ssh $N --no-pty '(Get-Disk | Where-Object PartitionStyle -eq "RAW").Number' 2>/dev/null | tail -1 | tr -d '\r ')
  "$W11" ssh $N --no-pty "powershell -NoProfile -File C:\\Users\\user\\$3 -Disk $d -Letter $2" >/dev/null 2>&1 || true
  echo "$d"
}
d=$(mk win_efs S guest_efs.ps1)
# EFS は対話セッションの鍵が要る (ssh では Access denied)。ssh 側で作った平文を desk 側で暗号化する
"$W11" desk $N 'cipher /e /a S:\secret_single.txt; cipher /e /a /s:S:\secret' >/dev/null 2>&1 || true
"$W11" ssh $N --no-pty 'Write-VolumeCache S' >/dev/null 2>&1 || true
"$W11" detach $N hp0 >/dev/null 2>&1 || true
d=$(mk win_bitlocker T guest_bitlocker.ps1)
"$W11" ssh $N --no-pty 'Write-VolumeCache T' >/dev/null 2>&1 || true
"$W11" detach $N hp0 >/dev/null 2>&1 || true
"$W11" ssh $N --no-pty "shutdown /s /t 0" >/dev/null 2>&1 || true
ls -la "$OUT"/win_efs.img "$OUT"/win_bitlocker.img
