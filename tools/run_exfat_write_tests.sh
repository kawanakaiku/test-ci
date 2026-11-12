#!/bin/sh
# exFAT 書き込みの検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/exfat-write.md)。自前ドライバ (fs/exfat を LinuxCompat 上で動かしたもの) で書いたイメージを、
# 独立実装 (exfatprogs の fsck.exfat と、relan の exfat-fuse = mount.exfat-fuse) で検証する。
#   1. 決まった操作列 (クラスタ 512B / 4KiB / 32KiB (セクタ 4096) のフィクスチャに対して): 自前ドライバで実行 → fsck.exfat -n が clean → 同じ操作列を exfat-fuse 上で
#      POSIX 操作として実行した結果とツリー全体 (種別・サイズ・sha256) が一致すること。
#      例外: partial_valid.bin (ValidDataLength より先への書き込み)。exfat-fuse は VDL と書き込み位置の間をゼロ埋めしない (古い値が見える) ので、規則から計算した期待値と比べる (tools/exfat_vdl_expect.py)。
#   2. 乱数操作 (メモリ上のモデルとの突き合わせ + 再マウント後の比較) をクラスタ 512B / 4KiB / 32KiB で → fsck.exfat clean、モデルのダンプが exfat-fuse の ro マウントのツリーと一致。
#   3. rw を要求しても何も書かないこと / 書き込み不可 (汚れた・MediaFailure) のイメージは読み取り専用でイメージ不変 / 満杯・ディレクトリ伸長不能で安全に失敗し回復 / 名前の規則 / クリーン unmount。
# 使い方: run_exfat_write_tests.sh <usbmem_exfat_write_tests> [フィクスチャ dir (c4k/c512/s4k_c32k.img。tools/gen_exfat_fixture.py)] [モデル試行数 (既定 4)]
# exfat-fuse (relan 1.4.0) の mount は fusermount3 3.x に拒否されるオプション (user= / blkdev / blksize=) を付けるので、起動直前に取り除く LD_PRELOAD (tools/exfat_fuse_shim.c) を使う。
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_exfat_${USER:-u}}; NM=${3:-4}
for c in mkfs.exfat fsck.exfat mount.exfat-fuse fusermount3 python3 gcc; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/c4k.img" ] && [ -f "$D/c512.img" ] && [ -f "$D/s4k_c32k.img" ] || { echo "skip: no fixture in $D (tools/gen_exfat_fixture.py)" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_xfw.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
gcc -shared -fPIC -O1 -o "$W/shim.so" "$HERE/exfat_fuse_shim.c" -ldl || { echo "skip: cannot build the fusermount3 shim" >&2; exit 77; }
fmount() {  # fmount <img> <mnt> <ro|rw>
  mkdir -p "$2"; LD_PRELOAD="$W/shim.so" mount.exfat-fuse -o "$3" "$1" "$2" 2>/dev/null
}
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0
fail() { echo "FAIL: $*"; rc=1; }
tree_of() {  # tree_of <img> <out>
  fmount "$1" "$W/mnt_t" ro || { echo "exfat-fuse ro mount failed: $1"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" > "$2"; fusermount3 -u "$W/mnt_t"
}
fsck_clean() {  # fsck_clean <img> <label>
  out=$(fsck.exfat -n "$1" 2>&1) || { echo "$out" | head -20; fail "$2: fsck.exfat not clean"; return 1; }
}

# ---------- 1. 決まった操作列 ----------
for fx in c512:512 c4k:4096 s4k_c32k:32768; do
  name=${fx%%:*}; cl=${fx##*:}; IMG=$D/$name.img
  python3 -I "$HERE/gen_exfat_ops.py" $cl "$W/ops_$name.txt"
  cp "$IMG" "$W/a_$name.img"; cp "$IMG" "$W/r_$name.img"
  out=$("$T" --ops "$W/a_$name.img" "$W/ops_$name.txt" 2>&1) || { echo "$out" | tail -5; fail "ops $name: driver"; continue; }
  echo "[$name] $(echo "$out" | grep -a '^ops:')"
  fsck_clean "$W/a_$name.img" "ops $name" || continue
  fmount "$W/r_$name.img" "$W/mnt_r" rw || { fail "exfat-fuse rw mount ($name)"; continue; }
  python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/ops_$name.txt" || { fusermount3 -u "$W/mnt_r"; fail "reference ops ($name)"; continue; }
  fusermount3 -u "$W/mnt_r"
  tree_of "$W/a_$name.img" "$W/ta" && tree_of "$W/r_$name.img" "$W/tr" || { fail "tree dump ($name)"; continue; }
  # VDL の例外: 自前ドライバの partial_valid.bin は規則どおりの期待値と一致 / 参照側は外す
  python3 -I "$HERE/exfat_vdl_expect.py" > "$W/vdl.exp"
  grep -F ' partial_valid.bin' "$W/ta" > "$W/vdl.got" || true
  cmp -s "$W/vdl.exp" "$W/vdl.got" || fail "ops $name: partial_valid.bin differs from the VDL rule"
  grep -v -F ' partial_valid.bin' "$W/ta" > "$W/ta2"; grep -v -F ' partial_valid.bin' "$W/tr" > "$W/tr2"
  if cmp -s "$W/ta2" "$W/tr2"; then echo "[$name] tree identical to reference ($(wc -l < "$W/ta") entries; partial_valid.bin by the VDL rule), fsck.exfat clean"; else diff "$W/ta2" "$W/tr2" | head -10; fail "ops $name: tree differs from exfat-fuse reference"; fi
done

# ---------- 2. 乱数操作 + モデル ----------
i=1
for cs in 512 4096 32768 4096; do
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    rm -f "$img"; truncate -s 96M "$img"; mkfs.exfat -q -c $cs "$img"
    out=$("$T" --model "$img" $seed 1200 "$W/m.dump" 2>&1) || { echo "$out" | tail -8; fail "model cluster=$cs seed=$seed"; continue; }
    fsck_clean "$img" "model cluster=$cs seed=$seed" || continue
    tree_of "$img" "$W/m.tree" || { fail "tree dump (model)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model cluster=$cs seed=$seed: exfat-fuse tree differs from model"; continue; }
    echo "[model cluster=$cs seed=$seed] $(echo "$out" | grep -a 'model:' | cut -c1-80); fsck.exfat clean; tree matches exfat-fuse"
  done
  i=$((i + 1))
done

# ---------- 2b. 数千ファイルの作成・削除 (大きなディレクトリ: クラスタをまたいで伸びる、消した跡の再利用) ----------
for cs in 512 4096; do
  img=$W/big.img; rm -f "$img"; truncate -s 64M "$img"; mkfs.exfat -q -c $cs "$img"
  python3 -I - "$W/many.ops" <<'PY'
import sys
L = ["mkdir /m"]
for i in range(3000):
    L += ["create /m/f%04d" % i, "write /m/f%04d 0 %d %d" % (i, (i * 37) % 6000, i % 251)]
for i in range(0, 3000, 2): L.append("unlink /m/f%04d" % i)            # 半分を消す
for i in range(1, 3000, 4): L += ["rename /m/f%04d /m/long_renamed_name_for_%04d_to_change_entry_count" % (i, i)]   # 名前の長さが変わる (必要なエントリ数が増える)
for i in range(3000, 4500):
    L += ["create /m/h%04d" % i, "write /m/h%04d 0 %d %d" % (i, (i * 13) % 3000, i % 199)]   # 消した跡 (エントリ・クラスタ) を再利用
for i in range(3, 3000, 4): L.append("unlink /m/f%04d" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
  cp "$img" "$W/big0.img"
  out=$("$T" --ops "$img" "$W/many.ops" 2>&1) || { echo "$out" | tail -5; fail "many files cluster=$cs: driver"; continue; }
  fsck_clean "$img" "many files cluster=$cs" || continue
  cp "$W/big0.img" "$W/bigr.img"
  if fmount "$W/bigr.img" "$W/mnt_r" rw; then python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/many.ops"; fusermount3 -u "$W/mnt_r"; fi
  tree_of "$img" "$W/ta" && tree_of "$W/bigr.img" "$W/tr" && cmp -s "$W/ta" "$W/tr" && echo "[many files cluster=$cs] $(echo "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to reference; fsck.exfat clean" || fail "many files cluster=$cs: tree differs"
done

# ---------- 3. 何も書かない / 書き込み拒否 / 満杯 / 名前 / クリーン unmount ----------
run_t() {  # run_t <label> <args...>: 失敗したら FAIL、最後の出力行を表示
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(echo "$out" | tail -1)"; else echo "$out" | tail -5; fail "$label"; fi
}
cp "$D/c4k.img" "$W/n.img"; run_t "noop (rw mount, no writes: image unchanged)" --noop "$W/n.img"
mk_small() { rm -f "$W/s.img"; truncate -s "${1:-8M}" "$W/s.img"; mkfs.exfat -q -c 4096 "$W/s.img"; }
setflags() {  # setflags <img> <value>: VolumeFlags (バイト 106-107。ブートチェックサムの対象外) を書き換える
  python3 -I -c "import struct,sys; f=open(sys.argv[1],'r+b'); f.seek(106); f.write(struct.pack('<H', int(sys.argv[2]))); f.close()" "$1" "$2"
}
mk_small; setflags "$W/s.img" 2; run_t "refuse unclean (VolumeDirty)" --refuse "$W/s.img"
mk_small; setflags "$W/s.img" 4; run_t "refuse MediaFailure" --refuse "$W/s.img"
mk_small; setflags "$W/s.img" 6; run_t "refuse VolumeDirty+MediaFailure" --refuse "$W/s.img"
mk_small 3M; run_t "enospc" --enospc "$W/s.img"
fsck_clean "$W/s.img" "enospc" || true
mk_small 16M; run_t "names" --names "$W/s.img"
fsck_clean "$W/s.img" "names" || true
mk_small 16M; run_t "clean unmount clears VolumeDirty" --clean "$W/s.img"
fsck_clean "$W/s.img" "clean" || true

[ $rc -eq 0 ] && echo "exfat write: all passed" || echo "exfat write: FAILED"
exit $rc
