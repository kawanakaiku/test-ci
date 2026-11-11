#!/bin/sh
# ext2 書き込みの検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/ext2-write.md)。自前ドライバ (fs/ext2 を LinuxCompat 上で動かしたもの) で書いたイメージを、
# 独立実装 (e2fsprogs の e2fsck と、libext2fs ベースの fuse2fs) で検証する。
#   1. 決まった操作列 (ブロックサイズ 1k/2k/4k のフィクスチャに対して): 自前ドライバで実行 → e2fsck -fn が clean → 同じ操作列を fuse2fs 上で POSIX 操作として実行した結果と
#      ツリー全体 (種別・サイズ・sha256・シンボリックリンク先) が一致すること。
#   2. 乱数操作 (メモリ上のモデルとの突き合わせ + 再マウント後の比較) → e2fsck clean、モデルのダンプが fuse2fs の ro マウントのツリーと一致。
#   3. rw を要求しても何も書かないこと / 書き込み不可 (汚れた・エラー・未知の機能) のイメージは読み取り専用でイメージ不変 / 満杯・inode 枯渇で安全に失敗し回復。
# 使い方: run_ext2_write_tests.sh <usbmem_ext2_write_tests> [フィクスチャ dir (b1k/b2k/b4k.img。tools/gen_ext2_fixture.py)] [モデル試行数 (既定 4)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_ext2_${USER:-u}}; NM=${3:-4}
for c in mke2fs e2fsck debugfs fuse2fs fusermount3 python3; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/b4k.img" ] || { echo "skip: no fixture in $D (tools/gen_ext2_fixture.py)" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_e2w.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0
fail() { echo "FAIL: $*"; rc=1; }
# イメージの ro マウントからツリーのダンプを作る (fuse2fs = e2fsprogs の libext2fs: 自前ドライバとは独立の実装)
tree_of() {  # tree_of <img> <out>
  mkdir -p "$W/mnt_t"; fuse2fs -o ro,fakeroot "$1" "$W/mnt_t" 2>/dev/null || { echo "fuse2fs ro mount failed: $1"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" > "$2"; fusermount3 -u "$W/mnt_t"
}
fsck_clean() {  # fsck_clean <img> <label>
  out=$(e2fsck -fn "$1" 2>&1) || { echo "$out" | head -20; fail "$2: e2fsck not clean"; return 1; }
}
sum_of() { sha256sum "$1" | cut -d" " -f1; }

# ---------- 1. 決まった操作列 ----------
gen_script() {  # gen_script <bs> <out>
python3 -I "$HERE/gen_ext2_ops.py" "$1" "$2"
}
for bsn in 1k 2k 4k; do
  bs=$(( ${bsn%k} * 1024 )); IMG=$D/b$bsn.img
  gen_script $bs "$W/ops_$bsn.txt"
  cp "$IMG" "$W/a_$bsn.img"; cp "$IMG" "$W/r_$bsn.img"
  out=$("$T" --ops "$W/a_$bsn.img" "$W/ops_$bsn.txt" 2>&1) || { echo "$out" | tail -5; fail "ops $bsn: driver"; continue; }
  echo "[$bsn] $(echo "$out" | grep -a '^ops:')"
  fsck_clean "$W/a_$bsn.img" "ops $bsn" || continue
  # 独立実装: 同じ操作列を fuse2fs 上で実行
  mkdir -p "$W/mnt_r"
  fuse2fs -o rw,fakeroot "$W/r_$bsn.img" "$W/mnt_r" 2>/dev/null || { fail "fuse2fs rw mount ($bsn)"; continue; }
  python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/ops_$bsn.txt" || { fusermount3 -u "$W/mnt_r"; fail "reference ops ($bsn)"; continue; }
  fusermount3 -u "$W/mnt_r"
  tree_of "$W/a_$bsn.img" "$W/ta" && tree_of "$W/r_$bsn.img" "$W/tr" || { fail "tree dump ($bsn)"; continue; }
  if cmp -s "$W/ta" "$W/tr"; then echo "[$bsn] tree identical to reference ($(wc -l < "$W/ta") entries), e2fsck clean"; else diff "$W/ta" "$W/tr" | head -10; fail "ops $bsn: tree differs from fuse2fs reference"; fi
done

# ---------- 2. 乱数操作 + モデル ----------
i=1
for bs in 1024 2048 4096 1024; do
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    rm -f "$img"; truncate -s 96M "$img"; mke2fs -q -t ext2 -b $bs "$img"
    out=$("$T" --model "$img" $seed 1500 "$W/m.dump" 2>&1) || { echo "$out" | tail -8; fail "model bs=$bs seed=$seed"; continue; }
    fsck_clean "$img" "model bs=$bs seed=$seed" || continue
    tree_of "$img" "$W/m.tree" || { fail "tree dump (model)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model bs=$bs seed=$seed: fuse2fs tree differs from model"; continue; }
    echo "[model bs=$bs seed=$seed] $(echo "$out" | grep -a 'model:' | cut -c1-80); e2fsck clean; tree matches fuse2fs"
  done
  i=$((i + 1))
done

# ---------- 2b. 数千ファイルの作成・削除 (大きな線形ディレクトリ、inode / ブロックの確保と解放の繰り返し) ----------
for bs in 1024 4096; do
  img=$W/big.img; rm -f "$img"; truncate -s 64M "$img"; mke2fs -q -t ext2 -b $bs "$img"
  python3 -I - "$W/many.ops" <<'PY'
import sys
L = ["mkdir /m"]
for i in range(3000):
    L += ["create /m/f%04d" % i, "write /m/f%04d 0 %d %d" % (i, (i * 37) % 6000, i % 251)]
for i in range(0, 3000, 2): L.append("unlink /m/f%04d" % i)            # 半分を消す
for i in range(1, 3000, 4): L += ["rename /m/f%04d /m/g%04d" % (i, i)]  # 一部の名前を変える (ディレクトリ内で長さが変わる)
for i in range(3000, 4500):
    L += ["create /m/h%04d" % i, "write /m/h%04d 0 %d %d" % (i, (i * 13) % 3000, i % 199)]   # 消した跡 (ディレクトリ・inode・ブロック) を再利用
for i in range(3, 3000, 4): L.append("unlink /m/f%04d" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
  cp "$img" "$W/big0.img"
  out=$("$T" --ops "$img" "$W/many.ops" 2>&1) || { echo "$out" | tail -5; fail "many files bs=$bs: driver"; continue; }
  fsck_clean "$img" "many files bs=$bs" || continue
  cp "$W/big0.img" "$W/bigr.img"; mkdir -p "$W/mnt_r"
  fuse2fs -o rw,fakeroot "$W/bigr.img" "$W/mnt_r" 2>/dev/null && python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/many.ops"; fusermount3 -u "$W/mnt_r"
  tree_of "$img" "$W/ta" && tree_of "$W/bigr.img" "$W/tr" && cmp -s "$W/ta" "$W/tr" && echo "[many files bs=$bs] $(echo "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to reference; e2fsck clean" || fail "many files bs=$bs: tree differs"
done

# ---------- 3. 何も書かない / 書き込み拒否 / 満杯 ----------
run_t() {  # run_t <label> <args...>: 失敗したら FAIL、最後の出力行を表示
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(echo "$out" | tail -1)"; else echo "$out" | tail -5; fail "$label"; fi
}
cp "$D/b4k.img" "$W/n.img"; run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
mk_small() { rm -f "$W/s.img"; truncate -s 16M "$W/s.img"; mke2fs -q -t ext2 -b 1024 "$@" "$W/s.img"; }
mk_small; debugfs -w -R "ssv state 0" "$W/s.img" >/dev/null 2>&1; run_t "refuse unclean (state=0)" --refuse "$W/s.img"
mk_small; debugfs -w -R "ssv state 3" "$W/s.img" >/dev/null 2>&1; run_t "refuse error state" --refuse "$W/s.img"
mk_small; debugfs -w -R "feature huge_file" "$W/s.img" >/dev/null 2>&1; run_t "refuse unknown ro_compat" --refuse "$W/s.img"
mk_small -N 200; run_t "enospc" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc" || true

[ $rc -eq 0 ] && echo "ext2 write: all passed" || echo "ext2 write: FAILED"
exit $rc
