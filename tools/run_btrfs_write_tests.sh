#!/bin/sh
# Btrfs 書き込みの検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/btrfs-write.md)。自前の書き込みエンジン (core/compat/src/btrfs_write.inc) で書いたイメージを、
# 独立実装 (btrfs-progs の btrfs check / btrfs restore / inspect-internal dump-tree) で検証する。USBMEM_BTRFS_KERNEL=1 なら実カーネル (loop マウント。sudo が要る) とも相互に確認する。
#   1. 決まった操作列 (mkfs.btrfs / --rootdir で作った各種イメージ): 自前ドライバで実行 → btrfs check --readonly --check-data-csum が clean →
#      同じ操作列を POSIX 操作として (btrfs restore で取り出した元のツリーに) 実行した結果と、btrfs restore で取り出した結果のツリー全体 (種別・サイズ・sha256・リンク先) が一致。
#   2. 乱数操作 + メモリ上のモデル (チェックサム 4 種・ノードサイズ・プロファイル・ブロックグループツリー有無 × seed。CreateOnly の操作だけの回も) → btrfs check clean、モデルのダンプが btrfs restore のツリーと一致。
#   3. 数千ファイルの作成・削除・rename。
#   4. rw を要求しても何も書かない / 書き込み不可 (未対応の機能・ミラー不一致・サブボリュームなど) は読み取り専用でイメージ不変 / 満杯。
#   5. 壊したイメージを書き込みマウントして操作する (ファジング)。
#   6. (USBMEM_BTRFS_KERNEL=1) 実カーネルとの相互確認。
# 使い方: run_btrfs_write_tests.sh <usbmem_btrfs_write_tests> [モデル試行数 (既定 2)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NM=${2:-2}
for c in mkfs.btrfs btrfs python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_btrfsw.XXXXXX)
cleanup() { if [ -n "${KMNT:-}" ]; then sudo -n umount "$KMNT" 2>/dev/null || true; fi; rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0; nfsck=0; nskip=0
fail() { echo "FAIL: $*"; rc=1; }
mk() {  # mk <img> <size> <mkfs opts...>
  img=$1; sz=$2; shift 2
  rm -f "$img"; truncate -s "$sz" "$img"
  if ! mo=$(mkfs.btrfs -q -f "$@" "$img" 2>&1); then
    # 古い btrfs-progs (Ubuntu の 6.6 など) には --compress / --subvol が無い: その構成だけ飛ばす (終了時に数える。他の失敗は失敗)
    if printf '%s\n' "$mo" | grep -qiE "unrecognized option|invalid option|unknown option|unknown feature"; then
      echo "skip (この mkfs.btrfs に無いオプション): $*" >&2; nskip=$((nskip + 1)); return 3
    fi
    echo "mkfs failed: $* : $(printf '%s\n' "$mo" | head -2)" >&2; return 1
  fi
}
tree_of() {  # tree_of <img> <out>: btrfs restore (独立実装) で取り出したツリーのダンプ
  d=$(mktemp -d "$W/rs.XXXXXX")
  btrfs restore -S -i "$1" "$d" >"$W/restore.log" 2>&1 || { tail -3 "$W/restore.log"; rm -rf "$d"; echo "btrfs restore failed: $1"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$d" > "$2"; rm -rf "$d"
}
fsck_clean() {  # fsck_clean <img> <label>
  nfsck=$((nfsck + 1))
  out=$(btrfs check --readonly --check-data-csum "$1" 2>&1) || { printf "%s\n" "$out" | head -30; fail "$2: btrfs check not clean"; return 1; }
  printf "%s\n" "$out" | grep -q "no error found" || { printf "%s\n" "$out" | head -30; fail "$2: btrfs check: no 'no error found'"; return 1; }
  return 0
}
run_t() {  # run_t <label> <args...>
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(printf "%s\n" "$out" | tail -1)"; else printf "%s\n" "$out" | tail -8; fail "$label"; fi
}
kernel_ok=0
if [ "${USBMEM_BTRFS_KERNEL:-0}" = 1 ] && sudo -n true 2>/dev/null; then kernel_ok=1; fi
kmount() {  # kmount <img>: loop マウント (KMNT に)
  KMNT=$W/kmnt; mkdir -p "$KMNT"; sudo -n mount -o loop "$1" "$KMNT" && sudo -n chown "$(id -u):$(id -g)" "$KMNT"
}
kumount() { sudo -n umount "$KMNT"; KMNT=; }

# ---------- 1. 決まった操作列 ----------
python3 -I "$HERE/gen_btrfs_ops.py" seed "$W/seed"
python3 -I "$HERE/gen_btrfs_ops.py" ops "$W/ops_fresh.txt"
python3 -I "$HERE/gen_btrfs_ops.py" ops "$W/ops_seeded.txt" seeded
fixed() {  # fixed <label> <size> <seeded: 0/1> <mkfs opts...>
  label=$1; sz=$2; sd=$3; shift 3
  if [ "$sd" = 1 ]; then mk "$W/a.img" "$sz" --rootdir "$W/seed" "$@" || { [ $? = 3 ] || fail "mkfs ($label)"; return 0; }; ops=$W/ops_seeded.txt
  else mk "$W/a.img" "$sz" "$@" || { [ $? = 3 ] || fail "mkfs ($label)"; return 0; }; ops=$W/ops_fresh.txt; fi
  tree_of "$W/a.img" "$W/t0" || { fail "restore original ($label)"; return 0; }
  fsck_clean "$W/a.img" "original ($label)" || return 0
  d=$W/ref; rm -rf "$d"; mkdir -p "$d"; btrfs restore -S -i "$W/a.img" "$d" >/dev/null 2>&1
  python3 -I "$HERE/ops_ref.py" "$d" "$ops" || { fail "reference ops ($label)"; return 0; }
  python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr"; rm -rf "$d"
  cp "$W/a.img" "$W/a0.img"
  out=$("$T" --ops "$W/a.img" "$ops" 2>&1) || { printf "%s\n" "$out" | tail -8; fail "ops $label: driver"; return 0; }
  fsck_clean "$W/a.img" "ops $label" || return 0
  tree_of "$W/a.img" "$W/ta" || { fail "restore result ($label)"; return 0; }
  if cmp -s "$W/ta" "$W/tr"; then echo "[$label] $(printf "%s\n" "$out" | grep -a '^ops:'); tree identical to POSIX reference ($(wc -l < "$W/ta") entries), btrfs check clean"
  else diff "$W/ta" "$W/tr" | head -10; fail "ops $label: tree differs from reference"; return 0; fi
  # 自前リーダーでも同じツリーが見えること
  "$T" --tree "$W/a.img" "$W/to" && cmp -s "$W/to" "$W/tr" || fail "ops $label: own reader tree differs"
  # 実カーネルで読めること・scrub / check が通ること
  if [ $kernel_ok = 1 ]; then
    cp "$W/a.img" "$W/k.img"
    if kmount "$W/k.img"; then
      python3 -I "$HERE/ops_ref.py" --tree "$KMNT" > "$W/tk"
      sudo -n btrfs scrub start -B -d "$KMNT" >"$W/scrub.log" 2>&1 || { tail -5 "$W/scrub.log"; fail "kernel scrub ($label)"; }
      kumount
      cmp -s "$W/tk" "$W/tr" || { diff "$W/tk" "$W/tr" | head; fail "kernel tree differs ($label)"; }
      fsck_clean "$W/k.img" "kernel-mounted ($label)" || true
      echo "[$label] kernel mount: tree identical, scrub ok"
    else fail "kernel mount failed ($label)"; fi
  fi
}
fixed "fresh crc32c" 128M 0
fixed "seeded crc32c" 160M 1
fixed "seeded xxhash n4k" 160M 1 --csum xxhash -n 4096
fixed "seeded sha256 n32k" 160M 1 --csum sha256 -n 32768
fixed "seeded blake2 n64k dup" 160M 1 --csum blake2 -n 65536 -m dup -d dup
fixed "seeded zstd (compressed extents)" 160M 1 --compress zstd
fixed "seeded zlib no-bgt" 160M 1 --compress zlib -O ^block-group-tree
fixed "seeded single/single" 160M 1 -m single -d single

# ---------- 2. 乱数操作 + モデル ----------
i=1
for cfg in "crc32c:16384:dup:full" "xxhash:4096:dup:full" "sha256:32768:single:full" "blake2:65536:dup:full" "crc32c:16384:dup:createonly" "crc32c:8192:single:nobgt"; do
  csum=$(echo $cfg | cut -d: -f1); ns=$(echo $cfg | cut -d: -f2); prof=$(echo $cfg | cut -d: -f3); mode=$(echo $cfg | cut -d: -f4)
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    extra=""
    [ "$mode" = nobgt ] && extra="-O ^block-group-tree"
    if [ "$prof" = dup ]; then pf="-m dup -d dup"; else pf="-m single -d single"; fi
    mk "$img" 256M --csum $csum -n $ns $pf $extra || { fail "mkfs model $cfg"; continue; }
    if [ $mode = createonly ]; then export USBMEM_MODEL_CREATEONLY=1; else unset USBMEM_MODEL_CREATEONLY || true; fi
    out=$("$T" --model "$img" $seed 1000 "$W/m.dump" 2>&1) || { printf "%s\n" "$out" | tail -8; fail "model $cfg seed=$seed"; continue; }
    unset USBMEM_MODEL_CREATEONLY || true
    fsck_clean "$img" "model $cfg seed=$seed" || continue
    tree_of "$img" "$W/m.tree" || { fail "tree dump (model)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model $cfg seed=$seed: btrfs restore tree differs from model"; continue; }
    echo "[model $csum n=$ns $prof $mode seed=$seed] $(printf "%s\n" "$out" | grep -a 'model:' | cut -c1-80); btrfs check clean; tree matches btrfs restore"
  done
  i=$((i + 1))
done

# ---------- 3. 数千ファイル ----------
mk "$W/big.img" 256M || fail "mkfs many"
python3 -I - "$W/many.ops" <<'PY'
import sys
L = ["mkdir /m"]
for i in range(2500):
    L += ["create /m/f%04d" % i, "write /m/f%04d 0 %d %d" % (i, (i * 37) % 6000, i % 251)]
for i in range(0, 2500, 2): L.append("unlink /m/f%04d" % i)
for i in range(1, 2500, 4): L += ["rename /m/f%04d /m/g%04d" % (i, i)]
for i in range(2500, 3500):
    L += ["create /m/h%04d" % i, "write /m/h%04d 0 %d %d" % (i, (i * 13) % 3000, i % 199)]
for i in range(3, 2500, 4): L.append("unlink /m/f%04d" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
tree_of "$W/big.img" "$W/t0" && d=$W/ref && rm -rf "$d" && mkdir -p "$d" && python3 -I "$HERE/ops_ref.py" "$d" "$W/many.ops" && python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr" && rm -rf "$d"
out=$("$T" --ops "$W/big.img" "$W/many.ops" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "many files: driver"; }
if fsck_clean "$W/big.img" "many files"; then
  tree_of "$W/big.img" "$W/ta" && cmp -s "$W/ta" "$W/tr" && echo "[many files] $(printf "%s\n" "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to reference; btrfs check clean" || fail "many files: tree differs"
fi

# ---------- 4. 何も書かない / 書き込み拒否 / 満杯 ----------
mk "$W/n.img" 128M --rootdir "$W/seed" && run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
refuse() {  # refuse <label> <mkfs opts...>
  label=$1; shift
  mk "$W/r.img" 128M "$@" || { fail "mkfs ($label)"; return 0; }
  run_t "refuse $label" --refuse "$W/r.img"
}
refuse "no-holes missing" -O ^no-holes
refuse "free-space-tree missing (v1 cache)" -R ^free-space-tree
refuse "skinny-metadata missing" -O ^skinny-metadata
refuse "mixed block groups" -M
mkdir -p "$W/sv/sub"; echo x > "$W/sv/sub/f"; echo y > "$W/sv/g"
mk "$W/r.img" 128M --rootdir "$W/sv" --subvol rw:sub; r=$?
if [ $r = 0 ]; then run_t "refuse subvolume present" --refuse "$W/r.img"; elif [ $r != 3 ]; then fail "mkfs subvol"; fi
mk "$W/r.img" 128M --rootdir "$W/sv" --subvol default:sub; r=$?
if [ $r = 0 ]; then run_t "refuse non-top-level default subvolume" --refuse "$W/r.img"; elif [ $r != 3 ]; then fail "mkfs default subvol"; fi
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" compat_ro=0x4b; run_t "refuse unknown compat_ro" --refuse "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" flags=0x800000001; run_t "refuse unknown sb flags" --refuse "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" gen_mirror=3; run_t "refuse superblock mirror generation mismatch" --refuse "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" bad_mirror; run_t "refuse corrupt superblock mirror" --refuse "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" dev_total=0x10000000; run_t "refuse device smaller than the filesystem" --refuse "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" incompat=0x100000361; run_t "no mount: unsupported incompat (simple quota)" --norw "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" incompat=0x1361; run_t "no mount: unknown incompat bit" --norw "$W/r.img"
mk "$W/r.img" 128M; python3 -I "$HERE/btrfs_patch_sb.py" "$W/r.img" log_root=0x1000000; run_t "no mount: pending tree-log (log_root != 0)" --norw "$W/r.img"
mk "$W/s.img" 128M; run_t "enospc" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc" || true
mk "$W/s.img" 128M -m single -d single; run_t "enospc (single)" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc (single)" || true

# ---------- 5. 壊したイメージを書き込みマウントして操作する (ファジング) ----------
mk "$W/fz.img" 64M --rootdir "$W/seed" --csum crc32c || fail "mkfs fuzz"
run_t "fuzz (rw, crc32c)" --fuzzw "$W/fz.img" ${USBMEM_BTRFS_FUZZ_N:-150} 1
mk "$W/fz.img" 64M --rootdir "$W/seed" --csum xxhash -n 4096 -m single -d single || fail "mkfs fuzz2"
run_t "fuzz (rw, xxhash n4k single)" --fuzzw "$W/fz.img" ${USBMEM_BTRFS_FUZZ_N:-150} 2

# ---------- 6. 実カーネルとの相互確認 ----------
if [ $kernel_ok = 1 ]; then
  # 6a. カーネルが書いたイメージを自前ドライバが読み・書き、結果をカーネルが読む
  for kcfg in "crc32c 16384" "xxhash 4096"; do
    set -- $kcfg
    mk "$W/k.img" 256M --csum $1 -n $2 || { fail "mkfs kernel $1"; continue; }
    kmount "$W/k.img" || { fail "kernel mount $1"; continue; }
    python3 -I "$HERE/ops_ref.py" "$KMNT" "$W/ops_fresh.txt" || fail "kernel ops ($1)"
    python3 -I - "$KMNT" <<'PY'
import os, sys
r = sys.argv[1]
for i in range(40):
    open(os.path.join(r, "w/kf%d" % i), "wb").write(bytes((j * 31 + i) & 255 for j in range(137 * i + 5)))
os.system("cp -a --reflink=always %s/w/big.bin %s/w/big_clone.bin 2>/dev/null || true" % (r, r))
PY
    sync; python3 -I "$HERE/ops_ref.py" --tree "$KMNT" > "$W/tk0"; kumount
    "$T" --tree "$W/k.img" "$W/to0" && cmp -s "$W/tk0" "$W/to0" || fail "own reader differs from kernel view ($1)"
    printf 'mkdir /w/after\ncreate /w/after/n1\nwrite /w/after/n1 0 70000 5\nwrite /w/big.bin 1000 200000 6\ntruncate /w/trunc.bin 1000\nunlink /w/hard.txt\nrename /w/many /w/many2\nwrite /w/kf3 0 9000 7\nunlink /w/kf5\n' > "$W/ops_after.txt"
    if out=$("$T" --ops "$W/k.img" "$W/ops_after.txt" 2>&1); then
      fsck_clean "$W/k.img" "ours after kernel ($1)" || continue
      kmount "$W/k.img" && { sudo -n btrfs scrub start -B -d "$KMNT" >/dev/null 2>&1 || fail "kernel scrub after ours ($1)"; python3 -I "$HERE/ops_ref.py" --tree "$KMNT" > "$W/tk1"; kumount; }
      "$T" --tree "$W/k.img" "$W/to1" && cmp -s "$W/tk1" "$W/to1" && echo "[kernel<->ours $1] kernel-written image modified by ours: check clean, kernel scrub ok, trees identical" || fail "kernel view after ours differs ($1)"
    else printf "%s\n" "$out" | tail -5; echo "[kernel<->ours $1] ours declined (read-only): $(printf '%s\n' "$out" | tail -1)"; fi
  done
fi

echo "btrfs check runs: $nfsck, skipped (mkfs.btrfs lacks option): $nskip"
[ $rc -eq 0 ] && echo "btrfs write: all passed" || echo "btrfs write: FAILED"
exit $rc
