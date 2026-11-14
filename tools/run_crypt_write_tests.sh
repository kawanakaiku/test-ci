#!/bin/bash
# 暗号ボリューム (LUKS / BitLocker) の書き込みの検証 (USBMEM_CRYPT_WRITE=ON ビルド用。docs/crypto-volumes.md「書き込み」)。
# 自前の実装で書いたイメージを、**自前ではない実装**で復号して、書いた内容 (平文のモデル) と全バイト比較する。
#   LUKS1/2 (xts-plain64 / cbc-essiv:sha256 / cbc-plain64、512 / 4096 セクタ、pbkdf2 / argon2id):
#     本物の `cryptsetup luksFormat` でヘッダを作り、Python (cryptography) で暗号化したイメージを自前で乱数書き込み →
#       (1) cryptsetup luksDump のマスターキーと Python cryptography だけで復号して、モデルと cmp
#       (2) USBMEM_KERNEL_DMCRYPT=1 (CI): 実カーネルの dm-crypt (sudo cryptsetup open on loop) で復号して、モデルと cmp
#       (3) ヘッダ領域が 1 バイトも変わっていない
#   BitLocker (6 方式: XTS-128/256, CBC-128/256, CBC+diffuser 128/256):
#     tools/bitlocker_synth.py (Python での独立した暗号化) で作ったイメージに自前で乱数書き込み →
#       (1) libbde の bdemount で復号してモデルと cmp、(2) 同じモデルを bitlocker_synth.py で暗号化し直したイメージと全バイト一致 (データ領域・退避領域)、
#       (3) メタデータ 3 コピーとブートセクタが不変、メタデータ領域への書き込みは ReadOnly で拒否
#   変換途中の BitLocker・非対応 IV の LUKS は、書き込みを要求しても読み取り専用で開き、イメージ不変 (norw)
#   下位の書き込み / flush の失敗 → 読み取り専用に落ちる (failinj)、壊れたヘッダ + 書き込み → クラッシュ・ヘッダ破壊なし (fuzzw)
# 使い方: run_crypt_write_tests.sh <usbmem_cryptwrite_tool> [作業ディレクトリ] 環境変数: USBMEM_CW_ONLY (luks / bl)、 USBMEM_CW_OPS (既定 500)、USBMEM_CW_SEEDS (既定 "1 2")、USBMEM_CW_FUZZ (既定 80)
# 必要なもの: cryptsetup, python3 (cryptography; BitLocker は numpy も)。無ければ SKIP (終了コード 77)。bdemount が無ければ libbde 比較を省く。
set -u
export PATH="$PATH:/sbin:/usr/sbin"
T=$1
HERE=$(cd "$(dirname "$0")" && pwd)
W=${2:-$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}/usbmem_cw.XXXXXX")}
mkdir -p "$W"
OPS=${USBMEM_CW_OPS:-500}; SEEDS=${USBMEM_CW_SEEDS:-"1 2"}; FUZZ=${USBMEM_CW_FUZZ:-80}
trap 'fusermount3 -u "$W/mnt" 2>/dev/null; [ "${USBMEM_KEEP:-0}" = 1 ] || rm -rf "$W"' EXIT
rc=0; n=0
fail() { echo "FAIL: $*"; rc=1; }
command -v python3 >/dev/null && python3 -c "import cryptography" 2>/dev/null || { echo "skip: python3 cryptography missing" >&2; exit 77; }
if ! command -v cryptsetup >/dev/null; then echo "skip: cryptsetup missing" >&2; exit 77; fi
CI=$HERE/crypt_indep.py
head -c 12582912 /dev/urandom > "$W/plain12.bin"
head -c 8388608 /dev/urandom > "$W/plain8.bin"
printf 'cw-test-passphrase-1' > "$W/pw"

kernel_check() {  # kernel_check <img> <model> : 実カーネルの dm-crypt で復号して cmp (CI のみ)
  [ "${USBMEM_KERNEL_DMCRYPT:-0}" = 1 ] || return 0
  local img=$1 model=$2 lo name=cw$$_$RANDOM size
  lo=$(sudo losetup -f --show "$img") || { fail "losetup $img"; return 1; }
  if sudo cryptsetup open --key-file "$W/pw" "$lo" "$name"; then
    size=$(stat -c %s "$model")
    if sudo cmp -n "$size" "/dev/mapper/$name" "$model"; then echo "     kernel dm-crypt (real cryptsetup open) == model ($size bytes)"; else fail "kernel dm-crypt differs from model: $img"; fi
    sudo cryptsetup close "$name"
  else fail "kernel cryptsetup open failed: $img"; fi
  sudo losetup -d "$lo"
}

# luks_case <name> <ver> <cipher> <bits> <hash> <sector> <pbkdf> <plain>
luks_case() {
  local name=$1 ver=$2 cipher=$3 bits=$4 hash=$5 sector=$6 pbkdf=$7 plain=$8 img="$W/$1.img" out off seed
  n=$((n+1))
  out=$(python3 -I "$CI" make-luks "$img" --plain "$W/$plain" --pwfile "$W/pw" --ver "$ver" --cipher "$cipher" --bits "$bits" --hash "$hash" --sector "$sector" --pbkdf "$pbkdf") || { fail "$name: make-luks: $out"; return; }
  off=$(echo "$out" | sed -n 's/^data_offset=\([0-9]*\).*/\1/p')
  python3 -I "$CI" decrypt-luks "$img" --pwfile "$W/pw" --out "$W/$name.dec0" >/dev/null || { fail "$name: indep decrypt (before)"; return; }
  [ "$(cmp -n "$(stat -c %s "$W/$plain")" "$W/$name.dec0" "$W/$plain" && echo same)" = same ] || { fail "$name: fixture self-check"; return; }
  "$T" info "$img" "pwfile:$W/pw" --writable | tr '\n' ' ' | sed 's/^/     /;s/ $//'; echo
  for seed in $SEEDS; do
    cp "$img" "$W/$name.w.img"
    res=$("$T" rand "$W/$name.w.img" "pwfile:$W/pw" "$seed" "$OPS" "$W/$name.model" --protect "0:$off" 2>&1) || { echo "$res" | tail -3; fail "$name seed=$seed: rand"; return; }
    python3 -I "$CI" decrypt-luks "$W/$name.w.img" --pwfile "$W/pw" --out "$W/$name.dec" >/dev/null || { fail "$name: indep decrypt (after)"; return; }
    if cmp "$W/$name.dec" "$W/$name.model"; then echo "ok   $name seed=$seed: $(echo "$res" | sed 's/^rand: ok //') ; independent decrypt (cryptsetup master key + python cryptography) == model"; else fail "$name seed=$seed: independent decrypt != model"; fi
    cmp -s "$W/$name.dec" "$W/$name.dec0" && fail "$name: writes did not change anything?"
    kernel_check "$W/$name.w.img" "$W/$name.model"
  done
  "$T" failinj "$W/$name.w.img" "pwfile:$W/pw" 5 --protect "0:$off" >/dev/null || fail "$name: failinj"
  "$T" fuzzw "$img" "pwfile:$W/pw" "$FUZZ" 3 --protect "0:$off" >/dev/null || fail "$name: fuzzw"
  "$T" ro "$W/$name.w.img" "pwfile:$W/pw" >/dev/null || fail "$name: ro (default must be read-only)"
  rm -f "$W/$name.dec" "$W/$name.dec0" "$W/$name.w.img" "$img" "$W/$name.model"
}

if [ "${USBMEM_CW_ONLY:-}" != bl ]; then   # USBMEM_CW_ONLY=luks|bl: 片方だけ (突然変異テストが使う)
luks_case l1_xts512      1 aes-xts-plain64     512 sha256 512  pbkdf2   plain12.bin
luks_case l1_xts256_sha1 1 aes-xts-plain64     256 sha1   512  pbkdf2   plain8.bin
luks_case l1_cbc_essiv   1 aes-cbc-essiv:sha256 256 sha256 512 pbkdf2   plain8.bin
luks_case l1_cbc_plain64 1 aes-cbc-plain64     128 sha256 512  pbkdf2   plain8.bin
luks_case l2_xts512      2 aes-xts-plain64     512 sha256 512  pbkdf2   plain12.bin
luks_case l2_xts256_s512 2 aes-xts-plain64     256 sha512 512  pbkdf2   plain8.bin
luks_case l2_xts_4096    2 aes-xts-plain64     512 sha256 4096 argon2id plain12.bin
luks_case l2_cbc_essiv   2 aes-cbc-essiv:sha256 256 sha256 512 pbkdf2   plain8.bin
luks_case l2_cbc_4096    2 aes-cbc-plain64     256 sha256 4096 pbkdf2   plain8.bin

# 書き込みを検証していない IV 方式 (32 ビット plain): 書き込みを要求しても読み取り専用 (解錠は成功)
for mode in aes-xts-plain aes-cbc-plain; do
  n=$((n+1)); bits=256
  img="$W/np_$mode.img"; head -c $((24<<20)) /dev/zero > "$img"
  if cryptsetup luksFormat --type luks1 --batch-mode --disable-locks --cipher "$mode" --key-size $bits --hash sha256 --key-file "$W/pw" --pbkdf-force-iterations 1000 "$img" >/dev/null 2>&1; then
    "$T" norw "$img" "pwfile:$W/pw" >/dev/null && echo "ok   luks1 $mode: write requested -> opened read-only, image unchanged" || fail "luks1 $mode norw"
  else echo "note: cryptsetup cannot format $mode here; skipped"; fi
  rm -f "$img"
done
fi

# ---------------- BitLocker (合成: bitlocker_synth.py)
if [ "${USBMEM_CW_ONLY:-}" = luks ]; then :
elif python3 -c "import numpy, cryptography" 2>/dev/null; then
  PL=12582912
  P=(--protect 0:8192 --protect $PL:65536 --protect $((PL+131072)):131072)          # ブート領域 (物理セクタ 0..15)・FVE メタデータ 3 コピー
  H=(--hidden $PL:65536 --hidden $((PL+65536)):8192 --hidden $((PL+131072)):65536 --hidden $((PL+196608)):65536)  # 0 に見えて書き込み拒否
  PWBL=synth-Password-1
  for m in 8004 8005 8002 8003 8000 8001; do
    n=$((n+1)); img="$W/bl$m.img"
    python3 -I "$HERE/bitlocker_synth.py" "$W/plain12.bin" "$img" --method $m --password "$PWBL" >/dev/null || { fail "bl$m: synth"; continue; }
    for seed in $SEEDS; do
      cp "$img" "$W/bl$m.w.img"
      res=$("$T" rand "$W/bl$m.w.img" "pw:$PWBL" "$seed" "$OPS" "$W/bl$m.model" "${P[@]}" "${H[@]}" 2>&1) || { echo "$res" | tail -3; fail "bl$m seed=$seed: rand"; continue; }
      # (2) 独立した暗号化 (bitlocker_synth.py) でモデルを暗号化し直したイメージとの一致: データ領域 [512, PL)、退避領域 (論理 0..15 セクタの置き場所)、メタデータ、ブートセクタ
      head -c $PL "$W/bl$m.model" > "$W/bl$m.p2"
      python3 -I "$HERE/bitlocker_synth.py" "$W/bl$m.p2" "$W/bl$m.re.img" --method $m --password "$PWBL" >/dev/null
      python3 -I - "$W/bl$m.re.img" "$W/bl$m.w.img" "$img" $PL <<'PYEOF' || fail "bl$m seed=$seed: written image != independent re-encryption"
import sys
a, b, o, PL = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read(), open(sys.argv[3], 'rb').read(), int(sys.argv[4])
vh = PL + 0x10000
assert len(a) == len(b) == len(o)
assert a[512:PL] == b[512:PL], "data area differs"
assert a[vh:vh + 8192] == b[vh:vh + 8192], "relocated volume header differs"
assert b[:512] == o[:512], "FVE boot sector changed"
assert b[PL:PL + 0x10000] == o[PL:PL + 0x10000] and b[PL + 0x20000:] == o[PL + 0x20000:], "metadata changed"
PYEOF
      echo "ok   bl$m seed=$seed: $(echo "$res" | sed 's/^rand: ok //') ; == independent python re-encryption of the model"
      # (1) libbde
      if command -v bdemount >/dev/null && command -v fusermount3 >/dev/null; then
        mkdir -p "$W/mnt"
        if bdemount -p "$PWBL" "$W/bl$m.w.img" "$W/mnt" >/dev/null 2>&1; then
          if cmp -n $PL "$W/mnt/bde1" "$W/bl$m.model"; then echo "     libbde bdemount output == model ($PL bytes)"; else fail "bl$m: libbde output != model"; fi
          fusermount3 -u "$W/mnt"
        else fail "bl$m: bdemount cannot open the written image"; fi
      fi
    done
    "$T" failinj "$W/bl$m.w.img" "pw:$PWBL" 5 "${P[@]}" >/dev/null || fail "bl$m: failinj"
    "$T" fuzzw "$img" "pw:$PWBL" "$FUZZ" 3 "${P[@]}" >/dev/null || fail "bl$m: fuzzw"
    "$T" ro "$W/bl$m.w.img" "pw:$PWBL" >/dev/null || fail "bl$m: ro"
    rm -f "$W/bl$m".* "$img"
  done
  # 変換途中 (暗号化が 50% まで): 書き込みを要求しても読み取り専用。イメージ不変。
  n=$((n+1)); img="$W/blp.img"
  python3 -I "$HERE/bitlocker_synth.py" "$W/plain12.bin" "$img" --method 8004 --password "$PWBL" --state 2,4 --enc-fraction 0.5 >/dev/null || fail "blp: synth"
  "$T" norw "$img" "pw:$PWBL" --allow-partial && echo "ok   bitlocker encryption-in-progress: write requested -> read-only, image unchanged" || fail "blp: norw"
  # TPM のみ・資格情報違い: 開けない (書き込み以前に拒否)
  rm -f "$img"
else
  echo "note: numpy missing; BitLocker part skipped" >&2
fi
echo "== $n write cases done"
[ "$n" -gt 0 ] || exit 77
exit $rc
