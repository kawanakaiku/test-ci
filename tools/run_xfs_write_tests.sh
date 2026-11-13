#!/bin/sh
# XFS 書き込みの検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/xfs-write.md)。自前の書き込みエンジン (core/compat/src/xfs_write*.inc) で書いたイメージを、
# 独立実装 (xfsprogs の xfs_repair -n / xfs_logprint、実カーネルの rw マウント = tools/xfs_vm/kreplay.py) で検証する。
#   1. 決まった操作列 (mkfs.xfs -p / 構成違い): 自前ドライバで実行 → xfs_repair -n が clean・ログが CLEAN → 同じ操作列を POSIX 操作として (元のツリーに) 実行した結果と、
#      自前リーダーのツリーと、実カーネルが見るツリー (全体: 種別・サイズ・sha256・リンク先) が一致。
#   2. 乱数操作 + メモリ上のモデル (構成 × seed) → xfs_repair -n clean、モデルのダンプが実カーネルのツリーと一致。
#   3. 数千ファイルの作成。
#   4. rw を要求しても何も書かない / 書き込み拒否 (V4・rmapbt・dirblklog>0・ストライプ・未知の機能・クォータなど) は読み取り専用でイメージ不変 / 満杯。
#   5. 壊したイメージを書き込みマウントして操作する (ファジング)。
#   6. 実カーネルが書いたイメージを自前ドライバが書き、結果をカーネルが読む。
# 使い方: run_xfs_write_tests.sh <usbmem_xfs_write_tests> [モデル試行数 (既定 2)]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/xfs_common.sh"
T=$1; NM=${2:-2}
for c in mkfs.xfs xfs_repair xfs_logprint xfs_db python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_xfsw.XXXXXX)
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0; nfsck=0; nskip=0; nkern=0
fail() { echo "FAIL: $*"; rc=1; }
KBACKEND=0
if python3 -I "$HERE/xfs_vm/kreplay.py" --probe >/dev/null 2>&1; then KBACKEND=1; fi
mk() {  # mk <img> <size> <proto> <mkfs opts...>
  img=$1; sz=$2; pr=$3; shift 3
  r=0; mk_xfs "$img" "$sz" "$pr" "$@" || r=$?
  if [ $r = 3 ]; then echo "skip (この mkfs.xfs に無いオプション): $*" >&2; nskip=$((nskip + 1)); return 3; fi
  return $r
}
fsck_clean() {  # fsck_clean <img> <label>
  nfsck=$((nfsck + 1))
  if ! xfs_clean "$1" > "$W/fsck.log" 2>&1; then cat "$W/fsck.log" | head -30; fail "$2: xfs_repair -n not clean"; return 1; fi
  xfs_logprint_ok "$1" || { fail "$2: log not CLEAN"; return 1; }
  return 0
}
run_t() {  # run_t <label> <args...>
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(printf "%s\n" "$out" | tail -1)"; else printf "%s\n" "$out" | tail -8; fail "$label"; fi
}
# ---- 実カーネルでの突き合わせ (イメージと期待するツリーをためて、まとめて VM / loop マウントで見る)
KQ=$W/kq; mkdir -p "$KQ"; kn=0; kq_list=""
kq_add() {  # kq_add <img> <expected tree> <label>
  [ $KBACKEND = 1 ] || return 0
  kn=$((kn + 1)); cp --sparse=always "$1" "$KQ/$kn.img"; cp "$2" "$KQ/$kn.exp"; echo "$3" > "$KQ/$kn.lbl"; kq_list="$kq_list $KQ/$kn.img"
  [ $kn -lt 10 ] || kq_flush
}
kq_flush() {
  [ -n "$kq_list" ] || return 0
  # shellcheck disable=SC2086
  python3 -I "$HERE/xfs_vm/kreplay.py" --post $kq_list >/dev/null 2>&1 || true
  for im in $kq_list; do
    b=${im%.img}; lbl=$(cat "$b.lbl"); nkern=$((nkern + 1))
    if [ "$(cat "$im.kres" 2>/dev/null)" != ok ]; then fail "kernel mount ($lbl): $(cat "$im.kres" 2>/dev/null)"; continue; fi
    cmp -s "$im.ktree" "$b.exp" || { diff "$im.ktree" "$b.exp" | head -6; fail "kernel tree differs ($lbl)"; continue; }
    fsck_clean "$im" "kernel mounted + wrote ($lbl)" || continue
    echo "[kernel $lbl] tree identical ($(wc -l < "$im.ktree") entries); kernel wrote more files into it; xfs_repair -n clean"
  done
  rm -rf "$KQ"; mkdir -p "$KQ"; kn=0; kq_list=""
}

# ---------- 1. 決まった操作列 ----------
python3 -I "$HERE/gen_xfs_ops.py" seed "$W/seed"
python3 -I "$HERE/gen_xfs_ops.py" ops "$W/ops_fresh.txt"
python3 -I "$HERE/gen_xfs_ops.py" ops "$W/ops_seeded.txt" seeded
fixed() {  # fixed <label> <size> <seeded: 0/1> <mkfs opts...>
  label=$1; sz=$2; sd=$3; shift 3
  if [ "$sd" = 1 ]; then pr=$W/seed; ops=$W/ops_seeded.txt; else pr=""; ops=$W/ops_fresh.txt; fi
  r=0; mk "$W/a.img" "$sz" "$pr" "$@" || r=$?
  [ $r = 0 ] || { [ $r = 3 ] || fail "mkfs ($label)"; return 0; }
  fsck_clean "$W/a.img" "original ($label)" || return 0
  d=$W/ref; rm -rf "$d"; if [ "$sd" = 1 ]; then cp -a "$W/seed" "$d"; else mkdir -p "$d"; fi
  python3 -I "$HERE/ops_ref.py" "$d" "$ops" || { fail "reference ops ($label)"; return 0; }
  python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr"; rm -rf "$d"
  out=$("$T" --ops "$W/a.img" "$ops" 2>&1) || { printf "%s\n" "$out" | tail -8; fail "ops $label: driver"; return 0; }
  fsck_clean "$W/a.img" "ops $label" || return 0
  "$T" --tree "$W/a.img" "$W/to" || { fail "own reader failed ($label)"; return 0; }
  if cmp -s "$W/to" "$W/tr"; then echo "[$label] $(printf "%s\n" "$out" | grep -a '^ops:'); own reader tree identical to POSIX reference ($(wc -l < "$W/to") entries), xfs_repair -n clean, log CLEAN"
  else diff "$W/to" "$W/tr" | head -10; fail "ops $label: tree differs from reference"; return 0; fi
  kq_add "$W/a.img" "$W/tr" "$label"
}
fixed "fresh default" 320M 0
fixed "seeded default" 320M 1
fixed "seeded isize=1024" 320M 1 -i size=1024
fixed "seeded agcount=2" 320M 1 -d agcount=2
fixed "seeded 2 GiB agcount=16" 2G 1 -d agcount=16
fixed "seeded finobt=0" 320M 1 -m finobt=0
fixed "seeded sparse=0" 320M 1 -i sparse=0
fixed "seeded bigtime=0 inobtcount=0 reflink=0" 320M 1 -m bigtime=0,inobtcount=0,reflink=0
fixed "seeded imaxpct=90" 320M 1 -i maxpct=90
fixed "seeded nrext64" 320M 1 -i nrext64=1
fixed "seeded 1 GiB" 1G 1

# ---------- 2. 乱数操作 + モデル ----------
i=1
for cfg in "default:" "agcount2:-d agcount=2" "agcount8_isize1024:-d agcount=8 -i size=1024" "nofinobt_nosparse:-m finobt=0 -i sparse=0"; do
  name=${cfg%%:*}; opts=${cfg#*:}
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    # shellcheck disable=SC2086
    r=0; mk "$img" 2G "" $opts || r=$?
    [ $r = 0 ] || { [ $r = 3 ] || fail "mkfs model $name"; continue; }
    out=$("$T" --model "$img" $seed 1000 "$W/m.dump" 2>&1) || { printf "%s\n" "$out" | tail -8; fail "model $name seed=$seed"; continue; }
    fsck_clean "$img" "model $name seed=$seed" || continue
    "$T" --tree "$img" "$W/m.tree" || { fail "tree dump (model)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model $name seed=$seed: tree differs from the model dump"; continue; }
    echo "[model $name seed=$seed] $(printf "%s\n" "$out" | grep -a 'model:' | cut -c1-80); xfs_repair -n clean; log CLEAN"
    kq_add "$img" "$W/m.dump" "model $name seed=$seed"
  done
  i=$((i + 1))
done
kq_flush

# ---------- 3. 数千ファイル ----------
r=0; mk "$W/big.img" 320M "" || r=$?
python3 -I - "$W/many.ops" <<'PY'
import sys
L = []
for d in range(10):
    L.append("mkdir /m%d" % d)
    for i in range(250):
        L += ["create /m%d/f%04d" % (d, i), "write /m%d/f%04d 0 %d %d" % (d, i, ((d * 250 + i) * 37) % 6000, i % 251)]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
d=$W/ref; rm -rf "$d"; mkdir -p "$d"; python3 -I "$HERE/ops_ref.py" "$d" "$W/many.ops" && python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr" && rm -rf "$d"
out=$("$T" --ops "$W/big.img" "$W/many.ops" 2>&1) || { printf "%s\n" "$out" | tail -5; fail "many files: driver"; }
if fsck_clean "$W/big.img" "many files"; then
  "$T" --tree "$W/big.img" "$W/ta" && cmp -s "$W/ta" "$W/tr" && echo "[many files] $(printf "%s\n" "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to reference; xfs_repair -n clean" || fail "many files: tree differs"
  kq_add "$W/big.img" "$W/tr" "many files"
fi
kq_flush

# ---------- 4. 何も書かない / 書き込み拒否 / 満杯 ----------
r=0; mk "$W/n.img" 320M "$W/seed" || r=$?
[ $r != 0 ] || run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
refuse() {  # refuse <label> <mkfs opts...>   (既定の rmapbt=0 / parent=0 を付けずに作る)
  label=$1; shift
  r=0; mk_xfs_plain "$W/r.img" 1G "" "$@" || r=$?
  [ $r != 3 ] || { echo "skip (この mkfs.xfs に無い構成): $label" >&2; nskip=$((nskip + 1)); return 0; }
  [ $r = 0 ] || { [ $r = 3 ] || fail "mkfs ($label)"; return 0; }
  run_t "refuse $label" --refuse "$W/r.img"
}
refuse "V4 (crc=0)" -m crc=0,rmapbt=0 -n ftype=1
refuse "dirblklog>0 (-n size=8192)" -n size=8192 -m rmapbt=0
refuse "rmapbt=1" -m rmapbt=1
refuse "stripe unit (-d su=64k,sw=2)" -d su=64k,sw=2 -m rmapbt=0
refuse "bs=1024 (V5 dir block > fs block)" -b size=1024 -m rmapbt=0
refuse "bs=2048 (V5 dir block > fs block)" -b size=2048 -m rmapbt=0
refuse "log stripe unit (-l su=64k)" -l su=64k -m rmapbt=0
refuse "ascii-ci (-n version=ci)" -n version=ci -m rmapbt=0
r=0; mk "$W/r.img" 320M "" || r=$?
if [ $r = 0 ]; then
  # スーパーブロックを書き換えて (CRC を付け直して) 作る拒否・マウント失敗のイメージ
  p() { cp --sparse=always "$W/r.img" "$W/r2.img"; python3 -I "$HERE/xfs_patch_sb.py" "$W/r2.img" "$@" || { fail "xfs_patch_sb $*"; return 1; }; }
  p qflags=0x7 && run_t "refuse quota enabled (qflags)" --refuse "$W/r2.img"
  p incompat=0x1000 && run_t "no mount: unknown incompat" --norw "$W/r2.img"
  p rextents=100 rblocks=100 && run_t "no mount: realtime" --norw "$W/r2.img"
  p log_incompat=0x1000 && run_t "refuse unknown log_incompat" --refuse "$W/r2.img"
fi
r=0; mk "$W/s.img" 320M "" || r=$?
[ $r != 0 ] || { run_t "enospc" --enospc "$W/s.img"; fsck_clean "$W/s.img" "enospc" || true; }
# 未再生のログ (電源断のあと): 書き込み・読み取りとも mount が失敗し、イメージは変わらない
r=0; mk "$W/d.img" 320M "$W/seed" || r=$?
if [ $r = 0 ]; then
  printf 'mkdir /a\ncreate /a/b\nwrite /a/b 0 300000 5\n' > "$W/d.ops"
  found=0
  for at in 70 74 78 80 82 84 86 90; do
    cp --sparse=always "$W/d.img" "$W/d2.img"; USBMEM_CRASH_AT=$at "$T" --ops "$W/d2.img" "$W/d.ops" >/dev/null 2>&1 || true
    if ! xfs_logprint_ok "$W/d2.img"; then found=1; run_t "no mount: dirty log (power loss at write $at; never replayed by us)" --norw "$W/d2.img"; break; fi
  done
  [ $found = 1 ] || fail "dirty-log image not produced"
fi

# ---------- 5. 壊したイメージを書き込みマウントして操作する (ファジング) ----------
r=0; mk "$W/fz.img" 320M "$W/seed" || r=$?
if [ $r = 0 ]; then
  run_t "fuzz (rw)" --fuzzw "$W/fz.img" ${USBMEM_XFS_FUZZ_N:-150} 1
  run_t "fuzz (rw, seed 2)" --fuzzw "$W/fz.img" ${USBMEM_XFS_FUZZ_N:-150} 2
fi

# ---------- 6. 実カーネルが書いたイメージを自前ドライバが書く ----------
if [ $KBACKEND = 1 ]; then
  r=0; mk "$W/k.img" 320M "$W/seed" || r=$?
  if [ $r = 0 ]; then
    python3 -I "$HERE/xfs_vm/kreplay.py" --post "$W/k.img" >/dev/null 2>&1 || true
    if [ "$(cat "$W/k.img.kres" 2>/dev/null)" = ok ]; then
      fsck_clean "$W/k.img" "kernel-written image" || true
      "$T" --tree "$W/k.img" "$W/to0" && echo "[kernel->ours] own reader reads the kernel-written image: $(wc -l < "$W/to0") entries"
      printf 'mkdir /after\ncreate /after/n1\nwrite /after/n1 0 70000 5\nwrite /kpost/k3 1000 200000 6\nwrite /tiny.txt 3 90000 7\ncreate /kpost/new\nwrite /kpost/new 0 1234 3\nsymlink /kpost/ln2 /x/y\n' > "$W/ops_after.txt"
      for i in $(seq 1 120); do echo "create /kpost/c$i" >> "$W/ops_after.txt"; done
      if out=$("$T" --ops "$W/k.img" "$W/ops_after.txt" 2>&1); then
        fsck_clean "$W/k.img" "ours after kernel" || true
        "$T" --tree "$W/k.img" "$W/to1"
        kq_add "$W/k.img" "$W/to1" "ours after kernel"
        kq_flush
      else printf "%s\n" "$out" | tail -5; fail "ours could not write the kernel-written image"; fi
    else fail "kernel could not mount the base image: $(cat "$W/k.img.kres" 2>/dev/null)"; fi
  fi
else echo "(実カーネルのバックエンドが無いため、カーネルとの突き合わせは省略)"; nskip=$((nskip + 1)); fi
kq_flush

echo "xfs_repair runs: $nfsck, kernel replays/mounts: $nkern, skipped: $nskip"
[ $rc -eq 0 ] && echo "xfs write: all passed" || echo "xfs write: FAILED"
exit $rc
