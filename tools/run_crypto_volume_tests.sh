#!/bin/sh
# 暗号ボリューム (LUKS / BitLocker) の結合検証。docs/crypto-volumes.md
# 使い方: run_crypto_volume_tests.sh <usbmem_cryptovol_tests> [fixture_dir]
#   LUKS:      tools/gen_luks_fixtures.py が作った <dir>/luks/cases.txt を全件検証 (復号 → 平文イメージとバイト比較 → FS を開いて全ツリー比較)
#   BitLocker: <dir>/winbl/ (tools/winvhd/make_bitlocker_matrix.sh) と /dev/shm/usbmem_winntfs_$USER/win_bitlocker.img (実 Windows) があれば検証
# フィクスチャが無ければ SKIP (終了コード 77)。tools/gen_luks_fixtures.py はこのスクリプトが無ければ自動で呼ぶ。
set -u
T=$1; D=${2:-/dev/shm/usbmem_crypt_${USER:-u}}
HERE=$(cd "$(dirname "$0")" && pwd)
rc=0; n=0
if [ ! -f "$D/luks/cases.txt" ] && command -v cryptsetup >/dev/null 2>&1 || [ ! -f "$D/luks/cases.txt" ] && [ -x /sbin/cryptsetup ]; then
  python3 -I "$HERE/gen_luks_fixtures.py" "$D/luks" >/dev/null 2>&1 || echo "note: LUKS fixture generation failed" >&2
fi
if [ -f "$D/luks/cases.txt" ]; then
  while IFS="$(printf '\t')" read -r name cred plain want desc; do
    img=$D/luks/${name%%#*}.img
    n=$((n+1))
    case "$want" in
      ok)
        out=$("$T" --dev-compare "$img" 0 0 "$cred" "$plain" 2>&1) || { echo "FAIL $name (dev-compare): $out" | tail -3; rc=1; continue; }
        out2=$("$T" --tree-compare "$img" 0 0 "$cred" "$plain" 2>&1) || { echo "FAIL $name (tree-compare): $out2" | tail -3; rc=1; continue; }
        echo "ok   $name  [$desc]  $(echo "$out2" | grep tree-compare)" ;;
      *)
        out=$("$T" --expect "$img" 0 0 "$cred" "$want" 2>&1) || { echo "FAIL $name (expected $want): $out"; rc=1; continue; }
        echo "ok   $name  -> $want  [$desc]" ;;
    esac
  done < "$D/luks/cases.txt"
else
  echo "skip: LUKS fixtures not available (need cryptsetup)" >&2
fi
[ "$n" -gt 0 ] && echo "== $n LUKS cases done"

# ---- BitLocker: 実 Windows 11 が作ったイメージ (MBR + NTFS 1 パーティション)。manifest = Windows 側で出した SHA-256 付き全ファイル一覧。
PW=usbmem-Test-pw-1
bl_part() { # <img>: パーティション先頭バイトと長さ (MBR 第 1 エントリ)
  s=$(od -An -tu4 -j454 -N4 "$1" | tr -d ' '); l=$(od -An -tu4 -j458 -N4 "$1" | tr -d ' ')
  echo "$((s*512)) $((l*512))"
}
bl_check() { # <name> <img> <cred> <manifest> <desc>
    cnm=$1; im=$2; cr=$3; mf=$4; ds=$5
  set -- $(bl_part "$im"); po=$1; pl=$2
  n=$((n+1))
  out=$("$T" --info "$im" "$po" "$pl" "$cr" 2>&1) || { echo "FAIL $cnm (open): $out" | tail -2; rc=1; return; }
  if [ -n "$mf" ] && [ -f "$mf" ]; then
    out=$("$T" --manifest "$im" "$po" "$pl" "$cr" "$mf" 2>&1) || { echo "FAIL $cnm (manifest): $(echo "$out" | tail -1)"; rc=1; return; }
    echo "ok   $cnm  [$ds]  $(echo "$out" | tail -1)"
  else
    echo "ok   $cnm  [$ds]  (open only)"
  fi
  if [ "${USBMEM_CV_QUICK:-0}" != 1 ] && command -v bdemount >/dev/null 2>&1 && command -v fusermount3 >/dev/null 2>&1; then
    mp=$D/mnt_$$; mkdir -p "$mp"
    if bdemount -o "$po" -p "$PW" "$im" "$mp" >/dev/null 2>&1; then
      out=$("$T" --dev-compare "$im" "$po" "$pl" "$cr" "$mp/bde1" 2>&1) || { echo "FAIL $cnm (dev-compare vs bdemount): $(echo "$out" | tail -1)"; rc=1; }
      echo "     $cnm  vs libbde bdemount: $(echo "$out" | tail -1)"
      fusermount3 -u "$mp" >/dev/null 2>&1
    fi
    rmdir "$mp" 2>/dev/null
  fi
}
bl_n=0
QUICK=${USBMEM_CV_QUICK:-0}   # 1 = サニタイザ版など遅いビルド向け: BitLocker は 2 イメージだけ検証し bdemount 比較を省く
for im in "$D"/winbl/bl_*.img /dev/shm/usbmem_winntfs_${USER:-u}/win_bitlocker.img; do
  [ -f "$im" ] || continue
  base=${im%.img}; mf=$base.manifest; nm=$(basename "$base")
  if [ "$QUICK" = 1 ]; then case $nm in bl_xts256full|bl_suspended) ;; *) continue ;; esac; fi
  # 実 Windows の手元フィクスチャ (hello.txt のみ) には manifest が無い
  bl_n=$((bl_n+1))
  if [ "$nm" = bl_partial ]; then
    # 途中で止めた実イメージ: 既定では開かない (Unsupported)。開く経路は合成イメージで検証 (docs/crypto-volumes.md)
    set -- $(bl_part "$im"); n=$((n+1))
    out=$("$T" --expect "$im" "$1" "$2" "pw:$PW" unsupported 2>&1) || { echo "FAIL $nm/password (expected unsupported): $out"; rc=1; continue; }
    echo "ok   $nm/password -> unsupported (encryption in progress)"
    continue
  fi
  bl_check "$nm/password" "$im" "pw:$PW" "$mf" "real Windows 11, password"
  rec=$(sed -n 's/^META recovery=//p' "$base.meta" 2>/dev/null | head -1)
  [ -n "$rec" ] && bl_check "$nm/recovery" "$im" "rec:$rec" "$mf" "real Windows 11, 48-digit recovery password"
  if grep -q '^META bek_b64=' "$base.meta" 2>/dev/null; then
    sed -n 's/^META bek_b64=//p' "$base.meta" | head -1 | base64 -d > "$D/winbl/_tmp.bek"
    bl_check "$nm/startup-key" "$im" "key:$D/winbl/_tmp.bek" "$mf" "real Windows 11, .BEK startup key"
    rm -f "$D/winbl/_tmp.bek"
  fi
  # 保護一時停止 (クリアキー): 資格情報なしで開けるはず
  if grep -q 'suspended' "$im" 2>/dev/null || [ "$(basename "$base")" = bl_suspended ]; then
    bl_check "$nm/clear-key" "$im" "none" "$mf" "real Windows 11, protection suspended (clear key)"
  fi
  # 違うパスワード: 通常は AuthFailed。保護一時停止 (クリアキー) なら資格情報なしで開く。暗号化途中は Unsupported (開かない)
  set -- $(bl_part "$im")
  n=$((n+1))
  case $(basename "$base") in
    bl_suspended) want=ok ;;
    bl_partial) want=unsupported ;;
    *) want=auth-failed ;;
  esac
  if [ "$want" = ok ]; then
    out=$("$T" --info "$im" "$1" "$2" "pw:wrong-password-xyz" 2>&1) || { echo "FAIL $nm/wrong-password: $out"; rc=1; continue; }
    echo "ok   $nm/wrong-password -> opens anyway (protection suspended: clear key)"
  else
    out=$("$T" --expect "$im" "$1" "$2" "pw:wrong-password-xyz" $want 2>&1) || { echo "FAIL $nm/wrong-password: $out"; rc=1; continue; }
    echo "ok   $nm/wrong-password -> $want"
  fi
done
[ "$bl_n" -gt 0 ] && echo "== $bl_n BitLocker images done"
[ "$n" -gt 0 ] || exit 77
exit $rc
