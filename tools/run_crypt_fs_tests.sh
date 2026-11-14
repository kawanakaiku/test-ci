#!/bin/bash
# 暗号ボリューム (書き込み可) の上で書き込み可の FS (ext4 / ext2 / NTFS / FAT) を動かす検証 (USBMEM_NTFS_WRITE=ON + USBMEM_CRYPT_WRITE=ON のビルド用。docs/crypto-volumes.md「書き込み」)。
#   平文の FS イメージを mkfs で作る → LUKS (cryptsetup luksFormat + Python 暗号化) または BitLocker (tools/bitlocker_synth.py) で暗号化 →
#   自前の暗号層 (writable) + 自前の FS ドライバで乱数操作 (既定: 新規作成のみ、--full: 上書き / truncate / unlink も) →
#   書き終えた暗号イメージを自前ではない実装で復号 (cryptsetup のマスターキー + Python cryptography / libbde bdemount / CI では実カーネルの dm-crypt) →
#   e2fsck -fn / ntfsfix -n / fsck.fat -n が clean、自前ドライバでの読み直しと、独立実装の FUSE (fuse2fs / ntfs-3g) でのツリーがモデルと一致。
#   書き込みを要求しない open では、FS の mount_rw も書き込み不可になり、イメージが 1 バイトも変わらない。
# 使い方: run_crypt_fs_tests.sh <usbmem_cryptfs_write_tests> [作業ディレクトリ]   環境変数: USBMEM_CFS_OPS (既定 150)、USBMEM_CFS_FS (既定 "ext4 ext2 ntfs fat")、USBMEM_KERNEL_DMCRYPT=1 (CI: 実カーネルの dm-crypt でも復号)
# 必要なもの: cryptsetup, python3 (cryptography, numpy)。無ければ SKIP (77)。mkfs / fsck / FUSE が無い FS だけ省く。
set -u
export PATH="$PATH:/sbin:/usr/sbin"
T=$1
HERE=$(cd "$(dirname "$0")" && pwd)
W=${2:-$(mktemp -d "${USBMEM_WORK_DIR:-/tmp}/usbmem_cfs.XXXXXX")}
mkdir -p "$W/m"
OPS=${USBMEM_CFS_OPS:-150}; FSS=${USBMEM_CFS_FS:-"ext4 ext2 ntfs fat"}
trap 'fusermount3 -u "$W/m" 2>/dev/null; [ "${USBMEM_KEEP:-0}" = 1 ] || rm -rf "$W"' EXIT
rc=0; n=0
fail() { echo "FAIL: $*"; rc=1; }
python3 -c "import cryptography, numpy" 2>/dev/null || { echo "skip: python3 cryptography/numpy missing" >&2; exit 77; }
command -v cryptsetup >/dev/null || { echo "skip: cryptsetup missing" >&2; exit 77; }
CI=$HERE/crypt_indep.py
SIZE=12582912
printf 'cfs-test-passphrase-1' > "$W/pw"
PWBL=synth-Password-1

mkplain() {  # mkplain <fs> <file> : 12 MiB の平文 FS イメージ。作れない FS は return 1
  local fs=$1 f=$2
  rm -f "$f"; truncate -s $SIZE "$f"
  case $fs in
    ext4) command -v mke2fs >/dev/null && command -v e2fsck >/dev/null || return 1
          mke2fs -q -t ext4 -b 4096 -I 256 -O has_journal,extent,huge_file,flex_bg,metadata_csum,64bit,dir_nlink,extra_isize,resize_inode,dir_index,sparse_super,large_file,ext_attr -F "$f" >/dev/null 2>&1 ;;
    ext2) command -v mke2fs >/dev/null && command -v e2fsck >/dev/null || return 1
          mke2fs -q -t ext2 -b 1024 -F "$f" >/dev/null 2>&1 ;;
    ntfs) command -v mkntfs >/dev/null && command -v ntfsfix >/dev/null || return 1
          # 4096 セクタの暗号ボリューム (ブロックサイズ 4096 に見える) の上の NTFS は、NTFS のセクタも 4096 でないとドライバが受け付けない
          mkntfs -F -f -Q -q -s "${3:-512}" -c 4096 -L cfs "$f" >/dev/null 2>&1 ;;
    fat)  command -v mkfs.fat >/dev/null && command -v fsck.fat >/dev/null || return 1
          mkfs.fat -F 16 -s 4 "$f" >/dev/null 2>&1 ;;
  esac
}

fsck_plain() {  # fsck_plain <fs> <file> <label>
  local fs=$1 f=$2 out
  case $fs in
    ext4|ext2) out=$(e2fsck -fn "$f" 2>&1) || { printf '%s\n' "$out" | head -15; fail "$3: e2fsck not clean"; return 1; } ;;
    ntfs) ntfsfix -n "$f" >/dev/null 2>&1 || { fail "$3: ntfsfix -n"; return 1; } ;;
    fat) out=$(fsck.fat -n "$f" 2>&1) || { printf '%s\n' "$out" | head -15; fail "$3: fsck.fat not clean"; return 1; } ;;
  esac
}

fuse_tree() {  # fuse_tree <fs> <file> <out> : 独立実装 (fuse2fs / ntfs-3g) の ro マウントからツリー。FAT は FUSE が無いので省く (return 2)
  local fs=$1 f=$2 out=$3
  case $fs in
    ext4|ext2) command -v fuse2fs >/dev/null || return 2; fuse2fs -o ro,fakeroot "$f" "$W/m" >/dev/null 2>&1 || return 1 ;;
    ntfs) command -v ntfs-3g >/dev/null || return 2; ntfs-3g -o ro "$f" "$W/m" >/dev/null 2>&1 || return 1 ;;
    *) return 2 ;;
  esac
  python3 -I "$HERE/ops_ref.py" --tree "$W/m" | grep -v -E ' lost\+found(/|$)' | sort > "$out"
  fusermount3 -u "$W/m"
}

verify_plain() {  # verify_plain <fs> <plain file> <model dump> <label> : 独立実装で復号した平文を検査
  local fs=$1 f=$2 model=$3 label=$4 ft
  fsck_plain "$fs" "$f" "$label" || return 1
  "$T" dump "$f" "$fs" "$W/dump.txt" || { fail "$label: own driver cannot read the independently decrypted image"; return 1; }
  sort "$model" > "$W/model.s"
  if ! diff <(sort "$W/dump.txt") "$W/model.s" >/dev/null; then diff <(sort "$W/dump.txt") "$W/model.s" | head -5; fail "$label: independently decrypted image != model (own driver reading)"; return 1; fi
  fuse_tree "$fs" "$f" "$W/fuse.txt"; ft=$?
  if [ $ft = 1 ]; then fail "$label: independent FUSE mount failed"; return 1; fi
  if [ $ft = 0 ]; then
    if ! diff "$W/fuse.txt" "$W/model.s" >/dev/null; then diff "$W/fuse.txt" "$W/model.s" | head -5; fail "$label: independent FUSE tree != model"; return 1; fi
    echo "     $label: e2fsck/ntfsfix/fsck clean; own reader and independent FUSE tree == model ($(wc -l < "$W/model.s") entries)"
  else
    echo "     $label: fsck clean; own reader == model ($(wc -l < "$W/model.s") entries)"
  fi
}

kernel_plain() {  # kernel_plain <img> <out> : 実カーネルの dm-crypt で復号したものを file にコピー (CI のみ)
  local img=$1 out=$2 lo name=cfs$$_$RANDOM
  lo=$(sudo losetup -f --show "$img") || return 1
  if sudo cryptsetup open --key-file "$W/pw" "$lo" "$name"; then
    sudo dd if=/dev/mapper/$name of="$out" bs=1M count=$((SIZE / 1048576)) status=none; sudo chown "$(id -u)" "$out"
    sudo cryptsetup close "$name"; sudo losetup -d "$lo"; return 0
  fi
  sudo losetup -d "$lo"; return 1
}

# case <fs> <enc: luks2xts|luks2xts4k|luks1cbc|bl8004|bl8001> <mode: "" | --full> <seed>
fs_case() {
  local fs=$1 enc=$2 mode=$3 seed=$4 label="$1/$2${3:+/full}" img="$W/c.img" cred model="$W/c.model" before
  n=$((n+1))
  mkplain "$fs" "$W/p.img" "$([ "$enc" = luks2xts4k ] && echo 4096 || echo 512)" || { echo "note: $label skipped (no mkfs/fsck for $fs)"; return; }
  case $enc in
    luks2xts)   python3 -I "$CI" make-luks "$img" --plain "$W/p.img" --pwfile "$W/pw" --ver 2 --cipher aes-xts-plain64 --bits 512 --sector 512 >/dev/null; cred="pwfile:$W/pw" ;;
    luks2xts4k) python3 -I "$CI" make-luks "$img" --plain "$W/p.img" --pwfile "$W/pw" --ver 2 --cipher aes-xts-plain64 --bits 512 --sector 4096 >/dev/null; cred="pwfile:$W/pw" ;;
    luks1cbc)   python3 -I "$CI" make-luks "$img" --plain "$W/p.img" --pwfile "$W/pw" --ver 1 --cipher aes-cbc-essiv:sha256 --bits 256 >/dev/null; cred="pwfile:$W/pw" ;;
    bl*)        python3 -I "$HERE/bitlocker_synth.py" "$W/p.img" "$img" --method "${enc#bl}" --password "$PWBL" >/dev/null; cred="pw:$PWBL" ;;
  esac || { fail "$label: encrypt fixture"; return; }
  [ -f "$img" ] || { fail "$label: encrypt fixture"; return; }
  # 読み取り専用 (既定): FS の mount_rw も書き込み不可、イメージは変わらない
  before=$(sha256sum < "$img")
  "$T" ro "$img" "$fs" "$cred" >/dev/null || { fail "$label: ro stack"; return; }
  [ "$before" = "$(sha256sum < "$img")" ] || { fail "$label: image changed although writable was not requested"; return; }
  res=$("$T" run "$img" "$fs" "$cred" "$seed" "$OPS" "$model" $mode 2>"$W/err.txt") || { grep -v -E 'unimplemented|EXT4-fs|^usbmem ' "$W/err.txt" | tail -8; fail "$label: run"; return; }
  echo "ok   $label seed=$seed: ${res#ok: }"
  # 独立実装での復号
  case $enc in
    luks*)
      python3 -I "$CI" decrypt-luks "$img" --pwfile "$W/pw" --out "$W/d.img" >/dev/null || { fail "$label: indep decrypt"; return; }
      head -c $SIZE "$W/d.img" > "$W/d2.img"; mv "$W/d2.img" "$W/d.img"
      verify_plain "$fs" "$W/d.img" "$model" "$label (cryptsetup master key + python cryptography)" || return
      if [ "${USBMEM_KERNEL_DMCRYPT:-0}" = 1 ]; then
        kernel_plain "$img" "$W/k.img" || { fail "$label: kernel dm-crypt"; return; }
        cmp "$W/k.img" "$W/d.img" || { fail "$label: kernel dm-crypt != python decrypt"; return; }
        verify_plain "$fs" "$W/k.img" "$model" "$label (real kernel dm-crypt)" || return
        if [ "$fs" = ext4 ] || [ "$fs" = fat ]; then  # 実カーネルの FS ドライバで ro マウントして、ツリーがモデルと一致
          sudo mount -o ro,loop "$W/k.img" "$W/m" && { python3 -I "$HERE/ops_ref.py" --tree "$W/m" | grep -v -E ' lost\+found(/|$)' | sort > "$W/kern.txt"; sudo umount "$W/m"; diff "$W/kern.txt" "$W/model.s" >/dev/null && echo "     $label: real kernel $fs driver tree == model" || fail "$label: kernel mount tree != model"; }
        fi
      fi ;;
    bl*)
      # 自前の層で復号した平文を、独立実装 (bitlocker_synth.py: numpy / cryptography) で暗号化し直し、書いた後のイメージと全バイト一致すること
      # (データ領域・退避したボリュームヘッダ。メタデータ 3 コピーとブートセクタは不変)。libbde は NTFS のボリュームヘッダしか開けないので、NTFS のときだけ bdemount でも復号する。
      "$T" decrypt "$img" "$cred" "$W/d.img" $SIZE || { fail "$label: own decrypt"; return; }
      python3 -I "$HERE/bitlocker_synth.py" "$W/d.img" "$W/re.img" --method "${enc#bl}" --password "$PWBL" >/dev/null || { fail "$label: re-encrypt"; return; }
      python3 -I - "$W/re.img" "$img" "$SIZE" <<'PYEOF' || { fail "$label: written image != independent re-encryption of its plaintext"; return; }
import sys
a, b, PL = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read(), int(sys.argv[3])
vh = PL + 0x10000
assert len(a) == len(b)
assert a[512:PL] == b[512:PL], "data area differs"
assert a[vh:vh + 8192] == b[vh:vh + 8192], "relocated volume header differs"
assert a[:512] == b[:512] and a[PL:PL + 0x10000] == b[PL:PL + 0x10000], "boot sector / metadata differs"
PYEOF
      echo "     $label: written image == independent python re-encryption of its plaintext"
      if [ "$fs" = ntfs ] && command -v bdemount >/dev/null; then
        bdemount -p "$PWBL" "$img" "$W/m" >/dev/null 2>&1 || { fail "$label: bdemount cannot open the written image"; return; }
        head -c $SIZE "$W/m/bde1" > "$W/d2.img"; fusermount3 -u "$W/m"
        cmp "$W/d2.img" "$W/d.img" || { fail "$label: libbde plaintext != own plaintext"; return; }
        echo "     $label: libbde bdemount plaintext == own plaintext"
      fi
      verify_plain "$fs" "$W/d.img" "$model" "$label (plaintext of written image)" || return ;;
  esac
  rm -f "$W/d.img" "$W/k.img" "$img" "$W/p.img"
}

for fs in $FSS; do
  fs_case "$fs" luks2xts "" 1
  fs_case "$fs" luks2xts4k "" 2
  fs_case "$fs" luks1cbc "" 3
  fs_case "$fs" bl8004 "" 4
done
# 上書き・truncate・unlink も含む Full (ext4 / NTFS)
for fs in ext4 ntfs; do
  case " $FSS " in *" $fs "*) fs_case "$fs" luks2xts --full 11; fs_case "$fs" bl8001 --full 12 ;; esac
done
echo "== $n crypt-fs cases done"
[ $rc = 0 ] && echo "ALL OK" || echo "FAILED"
exit $rc
