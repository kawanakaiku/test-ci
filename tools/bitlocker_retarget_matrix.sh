#!/bin/sh
# 実 Windows の BitLocker イメージを暗号方式違いへ変換 (tools/bitlocker_retarget.py) し、独立実装 libbde の bdemount の復号結果と
# usbmem の復号をバイト比較する。実 Windows では作れない Windows 7/8 方式 (AES-CBC + Elephant diffuser) も含む。docs/crypto-volumes.md
# 使い方: bitlocker_retarget_matrix.sh <usbmem_cryptovol_tests> <実 BitLocker イメージ> <ボリューム先頭オフセット> <パスワード> [作業dir]
# 必要: bdemount (libbde-utils), fusermount3, python3 + cryptography + numpy。無ければ SKIP (77)。
set -u
T=$1; SRC=$2; OFF=$3; PW=$4; W=${5:-/dev/shm/usbmem_crypt_${USER:-u}/retarget}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v bdemount >/dev/null 2>&1 || { echo "skip: bdemount not found" >&2; exit 77; }
command -v fusermount3 >/dev/null 2>&1 || { echo "skip: fusermount3 not found" >&2; exit 77; }
[ -f "$SRC" ] || { echo "skip: $SRC not found" >&2; exit 77; }
mkdir -p "$W"
rc=0
for spec in "8000 rec,clear" "8001 rec,clear" "8002 rec,clear" "8003 rec,clear" "8004 rec,clear" "8005 rec,clear"; do
  set -- $spec
  m=$1; add=$2
  img=$W/rt_$m.img
  mnt=$W/mnt_$m
  python3 -I "$HERE/bitlocker_retarget.py" "$SRC" "$OFF" "$PW" "$img" --method "$m" --add "$add" >/dev/null || { echo "FAIL $m: retarget"; rc=1; continue; }
  rec=$(sed -n 's/^recovery=//p' "$img.meta")
  mkdir -p "$mnt"
  bdemount -p "$PW" "$img" "$mnt" >/dev/null 2>&1 || { echo "FAIL $m: bdemount"; rc=1; continue; }
  out=$("$T" --dev-compare "$img" 0 0 "pw:$PW" "$mnt/bde1" 2>&1) && r1=ok || r1="FAIL($out)"
  out=$("$T" --dev-compare "$img" 0 0 "rec:$rec" "$mnt/bde1" 2>&1) && r2=ok || r2="FAIL($out)"
  out=$("$T" --dev-compare "$img" 0 0 "none" "$mnt/bde1" 2>&1) && r3=ok || r3="FAIL($out)"
  fusermount3 -u "$mnt" >/dev/null 2>&1
  echo "method $m: password=$r1 recovery=$r2 clearkey=$r3"
  case "$r1$r2$r3" in *FAIL*) rc=1 ;; esac
  rm -f "$img"
done
exit $rc
