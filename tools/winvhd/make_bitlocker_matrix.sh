#!/bin/sh
# 実 Windows 11 (w11 ツール) で BitLocker の各構成のイメージを作る (既知のファイルツリー入り)。
# 使い方: make_bitlocker_matrix.sh [出力ディレクトリ(既定 /dev/shm/usbmem_crypt_$USER/winbl)] [w11 のパス] [variant ...]
#   variant: xts256full cbc128 cbc256 startkey suspended partial   (省略で全部)
# 出力: <out>/bl_<variant>.img (MBR + NTFS を BitLocker で保護した生イメージ), <out>/bl_<variant>.manifest (Windows 側で計算した sha256 一覧),
#       <out>/bl_<variant>.meta (回復パスワード / .BEK (base64) / 保護の一覧 — テスト用の使い捨て値)
# ゲストは --ephemeral で起動し、終わったら必ず停止して破棄する。VM は同時に 1 つだけ。
set -eu
OUT=${1:-/dev/shm/usbmem_crypt_${USER:-u}/winbl}; W11=${2:-/run/media/u/Data/win11/w11}
VARIANTS="xts256full cbc128 cbc256 startkey suspended"
if [ $# -gt 2 ]; then shift 2; VARIANTS="$*"; fi
HERE=$(cd "$(dirname "$0")" && pwd)
[ -x "$W11" ] || { echo "skip: w11 not found" >&2; exit 77; }
mkdir -p "$OUT"; N=ubm$$
export VM_MEM=${VM_MEM:-4G} VM_DISPLAY=none
stop_vm() { "$W11" ssh $N --no-pty "shutdown /s /t 0" >/dev/null 2>&1 || true; }
trap stop_vm EXIT
( setsid "$W11" run $N --ephemeral >/dev/null 2>&1 & )
for i in $(seq 1 60); do "$W11" ps 2>/dev/null | grep -q "$N.*192\." && break; sleep 10; done
sleep 10
"$W11" ssh $N --no-pty 'whoami' >/dev/null 2>&1 || { echo "guest not reachable" >&2; exit 1; }
"$W11" cp "$HERE/guest_bitlocker_matrix.ps1" $N:C:/Users/user/guest_bitlocker_matrix.ps1 >/dev/null 2>&1
for v in $VARIANTS; do
  img="$OUT/bl_$v.img"; sz=256M; [ "$v" = partial ] && sz=1G
  rm -f "$img"; truncate -s $sz "$img"
  "$W11" attach $N "$img" --disk >/dev/null 2>&1; sleep 4
  d=$("$W11" ssh $N --no-pty '(Get-Disk | Where-Object PartitionStyle -eq "RAW").Number' 2>/dev/null | tail -1 | tr -d '\r ')
  "$W11" ssh $N --no-pty "powershell -NoProfile -File C:\\Users\\user\\guest_bitlocker_matrix.ps1 -Disk $d -Letter T -Variant $v" > "$OUT/bl_$v.raw" 2>"$OUT/bl_$v.err" || echo "variant $v: guest script failed" >&2
  sed -n '/^MANIFEST-BEGIN/,/^MANIFEST-END/p' "$OUT/bl_$v.raw" | sed '1d;$d' | tr -d '\r' > "$OUT/bl_$v.manifest"
  grep '^META ' "$OUT/bl_$v.raw" | tr -d '\r' > "$OUT/bl_$v.meta" || true
  "$W11" detach $N hp0 >/dev/null 2>&1 || true
  sleep 3
  echo "variant $v: $(wc -l < "$OUT/bl_$v.manifest") manifest lines"
done
stop_vm
trap - EXIT
ls -la "$OUT"
