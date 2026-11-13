#!/bin/sh
# F2FS 書き込み (チェックポイント) の検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/f2fs-write.md)。自前ドライバ (core/compat/src/f2fs_volume.cpp の書き込み) で書いたイメージを、
# 独立実装 (f2fs-tools の fsck.f2fs と、本物の Linux カーネルの f2fs) で検証する。
#   1. 決まった操作列 (フィクスチャ各種): 自前ドライバで実行 → fsck.f2fs が clean → カーネル (読み取り専用マウント) が見るツリー = 自前ドライバのツリー =
#      カーネルが同じ操作列を POSIX 操作として実行した結果のツリー。
#   2. 乱数操作 + メモリ上のモデル (機能の組み合わせ × seed。CreateOnly の操作だけの回も) → fsck.f2fs clean、カーネルのツリー = モデル。
#   3. 数千ファイルの作成・削除・rename。
#   4. 相互書き込み: 自前ドライバのイメージの上でカーネルが操作 → fsck.f2fs clean・自前ドライバの読みとカーネルのツリーが一致 →
#      そのカーネルが書いたイメージの上で自前ドライバが操作 → fsck.f2fs clean・カーネルのツリーと一致。
#   5. 何も書かない / 書き込み拒否 (暗号化・圧縮・casefold・verity・quota・要回復) / 満杯 / 壊したイメージのファジング。
# カーネル: sudo -n が使えて f2fs をループマウントできるならそれ (CI)、なければ QEMU/TCG の VM (tools/f2fs_vm/kcheck.py)、どちらも無ければカーネルの検査だけ省く (fsck.f2fs の検査は常に行う)。
# 使い方: run_f2fs_write_tests.sh <usbmem_f2fs_write_tests> [フィクスチャ dir (tools/gen_f2fs_fixture.py)] [モデル試行数 (既定 1)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_f2fs_${USER:-u}}; NM=${3:-1}
for c in mkfs.f2fs fsck.f2fs python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_f2w.XXXXXX)
cleanup() { if mountpoint -q "$W/kmnt" 2>/dev/null; then sudo -n umount "$W/kmnt" 2>/dev/null || true; fi; rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0; nfsck=0; nker=0
fail() { echo "FAIL: $*"; rc=1; }

# ---------- カーネルの選択 ----------
KMODE=${USBMEM_F2FS_KERNEL:-auto}
if [ "$KMODE" = auto ]; then
  KMODE=off
  if sudo -n true 2>/dev/null && { grep -qw f2fs /proc/filesystems || sudo -n modprobe f2fs 2>/dev/null; } && grep -qw f2fs /proc/filesystems && command -v losetup >/dev/null; then KMODE=loop
  elif python3 -I "$HERE/f2fs_vm/kcheck.py" --probe >/dev/null 2>&1; then KMODE=vm; fi
fi
echo "kernel cross-check: $KMODE"
mkdir -p "$W/kmnt"
# ktree <img> <out> [ops]: カーネルでマウントしてツリーを取る (ops があれば書き込み可でマウントして操作を実行してから)。カーネルが無ければ何もせず 0 (nker を増やさない)
ktree() {
  img=$1; out=$2; ops=${3:-}
  case $KMODE in
    off) return 0 ;;
    loop)
      if [ -n "$ops" ]; then sudo -n mount -t f2fs -o loop "$img" "$W/kmnt" || return 1; else sudo -n mount -t f2fs -o loop,ro "$img" "$W/kmnt" || return 1; fi
      if [ -n "$ops" ]; then sudo -n python3 -I "$HERE/ops_ref.py" "$W/kmnt" "$ops" || { sudo -n umount "$W/kmnt"; return 1; }; fi
      sudo -n python3 -I "$HERE/ops_ref.py" --tree "$W/kmnt" > "$out"; sudo -n umount "$W/kmnt" ;;
    vm)
      if [ -n "$ops" ]; then python3 -I "$HERE/f2fs_vm/kcheck.py" --ops "$ops" "$img" "$out"; else python3 -I "$HERE/f2fs_vm/kcheck.py" --ro "$img" "$out"; fi ;;
  esac
  nker=$((nker + 1))
}
fsck_clean() {  # fsck_clean <img> <label>  (--dry-run: 修正しない。clean でなければ rc != 0 か [Fail] が出る)
  nfsck=$((nfsck + 1))
  if ! fsck.f2fs --dry-run -f "$1" > "$W/fsck.log" 2>&1 || grep -q '\[Fail\|\[ASSERT\]\|\[FIX\]' "$W/fsck.log"; then
    grep '\[Fail\|\[ASSERT\]\|\[FIX\]\|Error' "$W/fsck.log" | head -8; fail "$2: fsck.f2fs not clean"; return 1
  fi
  return 0
}
run_t() {  # run_t <label> <args...>
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(printf "%s\n" "$out" | tail -1)"; else printf "%s\n" "$out" | tail -6; fail "$label"; fi
}
tree_cmp() {  # tree_cmp <a> <b> <label>
  if cmp -s "$1" "$2"; then return 0; fi
  diff "$1" "$2" | head -8; fail "$3: trees differ"; return 1
}
FEAT_MODERN=extra_attr,inode_checksum,sb_checksum,inode_crtime,lost_found
mk() {  # mk <img> <size> <features or ->  [その他の mkfs オプション]
  img=$1; sz=$2; ft=$3; shift 3
  rm -f "$img"; truncate -s "$sz" "$img"
  if [ "$ft" = "-" ]; then mkfs.f2fs -q -f "$@" "$img" >/dev/null 2>&1; else mkfs.f2fs -q -f -O "$ft" "$@" "$img" >/dev/null 2>&1; fi
}

# ---------- 1. 決まった操作列 ----------
python3 -I "$HERE/gen_ext2_ops.py" 4096 "$W/fixed.ops"
for fx in ext plain k_ext k_nofeat; do
  [ -f "$D/$fx.img" ] || { echo "(fixture $fx.img not found: skipped)"; continue; }
  cp "$D/$fx.img" "$W/a.img"
  if ! out=$("$T" --ops "$W/a.img" "$W/fixed.ops" 2>&1); then printf "%s\n" "$out" | tail -5; fail "ops $fx: driver"; continue; fi
  echo "[$fx] $(printf "%s\n" "$out" | grep -a '^ops:')"
  fsck_clean "$W/a.img" "ops $fx" || continue
  "$T" --dump "$W/a.img" "$W/ta" || { fail "dump ($fx)"; continue; }
  if [ "$KMODE" != off ]; then
    ktree "$W/a.img" "$W/tk" || { fail "kernel cannot mount our image ($fx)"; continue; }
    tree_cmp "$W/ta" "$W/tk" "ops $fx (kernel view of our image)" || continue
    # 参照: カーネルが同じ操作列を実行 (最初の 1 つの構成だけ。VM は遅い)
    if [ "$fx" = ext ]; then
      cp "$D/$fx.img" "$W/r.img"; ktree "$W/r.img" "$W/tr" "$W/fixed.ops" || { fail "kernel ops ($fx)"; continue; }
      tree_cmp "$W/ta" "$W/tr" "ops $fx (kernel executing the same ops)" || continue
      fsck_clean "$W/r.img" "kernel-written ($fx)" || true
    fi
  fi
  echo "[$fx] $(wc -l < "$W/ta") entries; fsck.f2fs clean${KMODE:+; kernel ($KMODE) sees the same tree}"
done

# ---------- 2. 乱数操作 + モデル ----------
i=1
for cfg in "$FEAT_MODERN" "-" "$FEAT_MODERN,flexible_inline_xattr" "extra_attr" "$FEAT_MODERN:createonly" "$FEAT_MODERN,flexible_inline_xattr:createonly"; do
  ft=${cfg%%:*}; mode=${cfg#*:}; [ "$mode" = "$cfg" ] && mode=full
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    mk "$img" 128M "$ft" || { fail "mkfs ($ft)"; continue; }
    if [ $mode = createonly ]; then export USBMEM_F2_CREATEONLY=1; else unset USBMEM_F2_CREATEONLY || true; fi
    out=$("$T" --model "$img" $seed 1200 "$W/m.dump" 2>&1) || { printf "%s\n" "$out" | tail -8; fail "model $cfg seed=$seed"; continue; }
    unset USBMEM_F2_CREATEONLY || true
    fsck_clean "$img" "model $cfg seed=$seed" || continue
    if [ "$KMODE" != off ]; then
      ktree "$img" "$W/m.tree" || { fail "kernel cannot mount (model $cfg)"; continue; }
      tree_cmp "$W/m.dump" "$W/m.tree" "model $cfg seed=$seed (kernel vs model)" || continue
    fi
    echo "[model $ft $mode seed=$seed] $(printf "%s\n" "$out" | grep -a 'model:' | cut -c1-80); fsck.f2fs clean${KMODE:+; kernel tree matches model}"
  done
  i=$((i + 1))
done

# ---------- 3. 数千ファイル ----------
python3 -I - "$W/many.ops" <<'PY'
import sys
L = ["mkdir /m"]
for i in range(3000):
    L += ["create /m/f%04d" % i, "write /m/f%04d 0 %d %d" % (i, (i * 37) % 6000, i % 251)]
for i in range(0, 3000, 2): L.append("unlink /m/f%04d" % i)
for i in range(1, 3000, 4): L += ["rename /m/f%04d /m/g%04d" % (i, i)]
for i in range(3000, 4500):
    L += ["create /m/h%04d" % i, "write /m/h%04d 0 %d %d" % (i, (i * 13) % 3000, i % 199)]
for i in range(3, 3000, 4): L.append("unlink /m/f%04d" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
mk "$W/big.img" 128M "$FEAT_MODERN"
if out=$("$T" --ops "$W/big.img" "$W/many.ops" 2>&1); then
  fsck_clean "$W/big.img" "many files" || true
  "$T" --dump "$W/big.img" "$W/ta"
  if [ "$KMODE" != off ]; then ktree "$W/big.img" "$W/tk" && tree_cmp "$W/ta" "$W/tk" "many files (kernel view)" || true; fi
  echo "[many files] $(printf "%s\n" "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries"
else printf "%s\n" "$out" | tail -5; fail "many files: driver"; fi

# ---------- 4. 相互書き込み ----------
if [ "$KMODE" != off ]; then
  for cfg in "$FEAT_MODERN" "$FEAT_MODERN,flexible_inline_xattr"; do
    mk "$W/x.img" 128M "$cfg"
    "$T" --model "$W/x.img" 777 500 "$W/x.dump" >/dev/null 2>&1 || { fail "cross: model"; continue; }
    "$T" --dump "$W/x.img" "$W/x0.tree"
    python3 -I "$HERE/gen_f2fs_followup_ops.py" "$W/x0.tree" 11 "$W/x1.ops" 100
    ktree "$W/x.img" "$W/x1.tree" "$W/x1.ops" || { fail "cross: kernel ops on our image"; continue; }
    fsck_clean "$W/x.img" "cross: after kernel wrote on our image" || continue
    "$T" --dump "$W/x.img" "$W/x1o.tree"; tree_cmp "$W/x1.tree" "$W/x1o.tree" "cross: our reader vs kernel tree" || continue
    python3 -I "$HERE/gen_f2fs_followup_ops.py" "$W/x1o.tree" 12 "$W/x2.ops" 100
    out=$("$T" --ops "$W/x.img" "$W/x2.ops" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "cross: our ops on kernel-written image"; continue; }
    fsck_clean "$W/x.img" "cross: after we wrote on the kernel's image" || continue
    "$T" --dump "$W/x.img" "$W/x2o.tree"; ktree "$W/x.img" "$W/x2.tree" || { fail "cross: kernel mount after our ops"; continue; }
    tree_cmp "$W/x2o.tree" "$W/x2.tree" "cross: kernel vs ours after ours-on-kernel" || continue
    echo "[cross $cfg] ours -> kernel -> ours: fsck.f2fs clean at each step; trees identical ($(wc -l < "$W/x2o.tree") entries)"
  done
fi

# ---------- 5. 何も書かない / 書き込み拒否 / 満杯 / ファジング ----------
mk "$W/n.img" 64M "$FEAT_MODERN"
run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
for ft in "$FEAT_MODERN,encrypt" "$FEAT_MODERN,verity" "$FEAT_MODERN,quota" "$FEAT_MODERN,project_quota,quota" "$FEAT_MODERN,compression" "$FEAT_MODERN,ro"; do
  if mk "$W/x.img" 64M "$ft"; then run_t "refuse ($ft)" --refuse "$W/x.img"; else echo "(mkfs -O $ft unsupported: skipped)"; fi
done
if mk "$W/x.img" 64M "$FEAT_MODERN,casefold" -C utf8; then run_t "refuse (casefold)" --refuse "$W/x.img"; else echo "(mkfs casefold unsupported: skipped)"; fi
for fx in k_comp_lz4_c2 k_comp_zstd_c2 quota k_unclean; do
  [ -f "$D/$fx.img" ] || continue
  cp "$D/$fx.img" "$W/x.img"; run_t "refuse fixture $fx" --refuse "$W/x.img"
done
mk "$W/s.img" 48M "$FEAT_MODERN"; run_t "enospc" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc" || true
mk "$W/fz.img" 48M "$FEAT_MODERN"
python3 -I - "$W" <<'PY'
import os, sys
w = sys.argv[1]; os.makedirs(os.path.join(w, "fzt/d"), exist_ok=True)
for i in range(30): open(os.path.join(w, "fzt/f%d" % i), "wb").write(bytes((j * 31 + i) & 255 for j in range(100 * i + 5)))
for i in range(10): open(os.path.join(w, "fzt/d/g%d" % i), "wb").write(bytes((j * 7 + i) & 255 for j in range(70000)))
PY
sload.f2fs -f "$W/fzt" "$W/fz.img" >/dev/null 2>&1 || true
run_t "fuzz (rw)" --fuzzw "$W/fz.img" 200 1

echo "fsck.f2fs checks: $nfsck; kernel ($KMODE) cross-checks: $nker"
[ $rc -eq 0 ] && echo "f2fs write: all passed" || echo "f2fs write: FAILED"
exit $rc
