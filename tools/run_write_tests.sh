#!/bin/sh
# 実験的な書き込み経路の検証 (USBMEM_NTFS_WRITE=ON ビルド用)。自前ドライバで書いたイメージを、独立実装の ntfsfix -n / ntfs-3g で検証する。
# 期待値は「元イメージのファイル内容 + 書いた範囲のパターン (+ 拡張時の隙間はゼロ)」を計算し、ファイル全体を比較する。他ファイルは manifest で確認。
# 使い方: run_write_tests.sh <usbmem_ntfs_tests> [fixture dir (ntfs_basic.img)]
set -eu
T=$1; D=${2:-/dev/shm/usbmem_ntfs_${USER:-u}}
command -v ntfsfix >/dev/null && command -v ntfs-3g >/dev/null || { echo "skip: ntfs-3g tools missing" >&2; exit 77; }
[ -f "$D/ntfs_basic.img" ] || { echo "skip: no fixture in $D" >&2; exit 77; }
W=$(mktemp -d /dev/shm/usbmem_wt.XXXXXX); trap 'fusermount3 -u "$W/m" 2>/dev/null || true; fusermount3 -u "$W/o" 2>/dev/null || true; rm -rf "$W"' EXIT
mkdir -p "$W/m" "$W/o"
IMG=$D/ntfs_basic.img
rc=0
ntfs-3g -o ro "$IMG" "$W/o" 2>/dev/null || { echo "cannot mount original"; exit 1; }
check() {  # check <mode: inplace|iter> <path> <off> <len> <seed>
  mode=$1; path=$2; off=$3; len=$4; seed=$5
  cp "$IMG" "$W/w.img"
  if [ "$mode" = iter ]; then export USBMEM_WRITE_ITER=1; else unset USBMEM_WRITE_ITER || true; fi
  out=$("$T" --wtest "$W/w.img" "$path" "$off" "$len" "$seed" 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true)
  [ -z "$out" ] || { echo "FAIL[$mode $path $off $len] driver: $out" | cut -c1-200; return 1; }
  ntfsfix -n "$W/w.img" >/dev/null 2>&1 || { echo "FAIL[$mode $path $off $len] ntfsfix"; return 1; }
  ntfs-3g -o ro "$W/w.img" "$W/m" 2>/dev/null || { echo "FAIL[$mode $path $off $len] ntfs-3g mount"; return 1; }
  python3 -I - "$W/o/$path" "$W/m/$path" "$off" "$len" "$seed" <<'PY' || { fusermount3 -u "$W/m"; echo "FAIL[$mode $path $off $len] content"; return 1; }
import sys, os
orig, got, off, ln, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
exp = bytearray(open(orig, 'rb').read()) if os.path.exists(orig) else bytearray()
if len(exp) < off + ln: exp += bytes(off + ln - len(exp))
for i in range(off, off + ln): exp[i] = ((i * 7) + seed) & 255
data = open(got, 'rb').read()
if data != bytes(exp):
    d = next((i for i in range(min(len(data), len(exp))) if data[i] != exp[i]), None)
    print("size got/exp:", len(data), len(exp), "first diff:", d); sys.exit(1)
PY
  fusermount3 -u "$W/m"
  return 0
}
# --- 既存範囲の上書き (write_inplace と write_iter): 1 バイト / ページ境界 / 複数ページ / EOF 直前
check inplace big.bin 0 1 1 || rc=1
check inplace big.bin 100000 70000 3 || rc=1
check iter big.bin 0 1 1 || rc=1
check iter big.bin 4095 2 2 || rc=1
check iter big.bin 100000 70000 3 || rc=1
check iter big.bin 1048575 1048578 5 || rc=1
# --- 拡張: EOF 直前から / EOF ちょうどから / 隙間 (ゼロ埋め) / ページ・クラスタ境界 / 数 MiB
check iter big.bin 3146000 5000 4 || rc=1
check iter big.bin 3146505 100 6 || rc=1
check iter big.bin 3200000 1000 7 || rc=1
check iter big.bin 4194304 4096 8 || rc=1
check iter big.bin 3146505 3000000 9 || rc=1
# --- 小クラスタ向けの境界 (512/1024/2048/4096 の境目、隙間、新規ファイルの途中書き)
check iter big.bin 2047 2 20 || rc=1
check iter big.bin 2048 1 21 || rc=1
check iter big.bin 511 3000 22 || rc=1
check iter empty.txt 2500 10 23 || rc=1
check iter empty.txt 1000 3000 24 || rc=1
check iter a.txt 2047 3 25 || rc=1
check iter a.txt 6000 1 26 || rc=1
# --- 小さな常駐ファイル: 内部上書き / 常駐のまま拡張 / 非常駐へ昇格 / 空ファイルへ書く / 隙間付き
check iter a.txt 0 3 10 || rc=1
check iter a.txt 6 50 11 || rc=1
check iter a.txt 0 5000 12 || rc=1
check iter empty.txt 0 10 13 || rc=1
check iter empty.txt 0 20000 14 || rc=1
check iter a.txt 9000 100 15 || rc=1
# --- 名前空間の変更 (create/mkdir/unlink/rmdir/rename + 書き込み): 同じ操作列を ntfs-3g の POSIX で実行したリファレンスと全ツリー (名前・サイズ・sha256) を比較
ntfs_ops() {  # ntfs_ops <名前> <script ファイル>
  name=$1; sc=$2
  cp "$IMG" "$W/w.img"
  raw=$("$T" --ops "$W/w.img" "$sc" 2>&1 | grep -v unimplemented || true)
  out=$(echo "$raw" | grep -v "^ops:" | grep -v "^msc ops:" || true)
  [ -z "$out" ] || { echo "FAIL[$name] driver: $out" | cut -c1-200; return 1; }
  if [ -n "${USBMEM_VIA_MSC:-}" ]; then   # USB Mass Storage (模擬 BOT) 越し: 送った SCSI opcode は読み書き・同期・初期化の既知のものだけで、書き込みと同期が実際に出ていること
    echo "$raw" | grep "^msc ops:" | python3 -I -c "
import sys, re
l = sys.stdin.read()
ops = dict(re.findall(r'([0-9a-f]{2})x(\d+)', l.split('(')[0]))
allowed = {'00','12','1a','25','9e','28','88','2a','8a','35','03'}
bad = sorted(set(ops) - allowed)
if bad or not ({'2a','8a'} & set(ops)) or '35' not in ops: print('unexpected SCSI ops:', l.strip()); sys.exit(1)
" || { echo "FAIL[$name] SCSI opcodes"; return 1; }
  fi
  ntfsfix -n "$W/w.img" >/dev/null 2>&1 || { echo "FAIL[$name] ntfsfix"; return 1; }
  cp "$IMG" "$W/ref.img"; mkdir -p "$W/r"
  ntfs-3g -o rw "$W/ref.img" "$W/r" 2>/dev/null || { echo "FAIL[$name] ref mount"; return 1; }
  python3 -I "$HERE/ops_ref.py" "$W/r" "$sc" || { fusermount3 -u "$W/r"; echo "FAIL[$name] ref ops"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/r" > "$W/ref.tree"; fusermount3 -u "$W/r"
  ntfs-3g -o ro "$W/w.img" "$W/m" 2>/dev/null || { echo "FAIL[$name] ntfs-3g mount"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/m" > "$W/our.tree"; fusermount3 -u "$W/m"
  cmp -s "$W/ref.tree" "$W/our.tree" || { echo "FAIL[$name] tree differs:"; diff "$W/ref.tree" "$W/our.tree" | head -6 | cut -c1-160; return 1; }
  return 0
}
HERE=$(cd "$(dirname "$0")" && pwd)
cat > "$W/s1" <<'OPS'
create /n1.txt
write /n1.txt 0 100 1
mkdir /d1
create /d1/f.txt
write /d1/f.txt 0 10000 2
mkdir /d1/sub
create /d1/sub/deep.bin
write /d1/sub/deep.bin 0 300000 3
OPS
ntfs_ops create_mkdir "$W/s1" || rc=1
cat > "$W/s2" <<'OPS'
unlink /a.txt
unlink /empty.txt
unlink /big.bin
rmdir /dir1/sub
unlink /dir1/b.txt
rmdir /dir1
create /big.bin
write /big.bin 0 700000 4
OPS
ntfs_ops delete_recreate "$W/s2" || rc=1
cat > "$W/s3" <<'OPS'
rename /a.txt /a_renamed.txt
rename /dir1 /dir1_renamed
rename /dir1_renamed/b.txt /moved_b.txt
mkdir /dest
rename /big.bin /dest/big_moved.bin
rename /empty.txt /dest/e.txt
OPS
ntfs_ops rename "$W/s3" || rc=1
python3 - "$W/s4" <<'PY'
import sys
L = ["mkdir /bulk"]
for i in range(1, 1201): L += ["create /bulk/file_%d.txt" % i, "write /bulk/file_%d.txt 0 %d %d" % (i, 10 + i % 700, i % 250)]
for i in range(1, 1201, 3): L.append("unlink /bulk/file_%d.txt" % i)
for i in range(1, 801, 2): L.append("unlink /many/file_%d.txt" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
ntfs_ops many_files "$W/s4" || rc=1
python3 - "$W/s5" <<'PY'
import sys
L = ["create /日本語の新しいファイル.txt", "write /日本語の新しいファイル.txt 0 5000 9", "mkdir /新しいディレクトリ", "create /新しいディレクトリ/中.txt", "write /新しいディレクトリ/中.txt 0 77 5",
     "create /long_" + "y" * 240 + ".txt", "write /long_" + "y" * 240 + ".txt 0 300 6", "create /with space (1).txt", "rename /日本語の新しいファイル.txt /リネーム後.txt"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
ntfs_ops unicode_names "$W/s5" || rc=1
# --- 上流が未対応の操作 (圧縮ファイルの削除) は失敗してもツリーを変えず、ファイルシステムも整合していること
echo "unlink /cdir/random.bin" > "$W/s6"
cp "$IMG" "$W/w.img"
if "$T" --ops "$W/w.img" "$W/s6" >/dev/null 2>&1; then echo "FAIL[unsupported_delete] unexpectedly succeeded"; rc=1; else
  ntfsfix -n "$W/w.img" >/dev/null 2>&1 || { echo "FAIL[unsupported_delete] ntfsfix"; rc=1; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/o" > "$W/before.tree"
  ntfs-3g -o ro "$W/w.img" "$W/m" 2>/dev/null && python3 -I "$HERE/ops_ref.py" --tree "$W/m" > "$W/after.tree"; fusermount3 -u "$W/m" 2>/dev/null || true
  cmp -s "$W/before.tree" "$W/after.tree" || { echo "FAIL[unsupported_delete] tree changed"; rc=1; }
fi
cat > "$W/s8" <<'OPS'
truncate /a.txt 0
truncate /big.bin 100000
truncate /empty.txt 5000
create /t1.bin
write /t1.bin 0 50000 3
truncate /t1.bin 10
truncate /t1.bin 300000
write /t1.bin 299990 20 4
truncate /dir1/b.txt 1
truncate /sparse.bin 4096
truncate /many/file_1.txt 70000
OPS
ntfs_ops truncate "$W/s8" || rc=1
cat > "$W/s9" <<'OPS'
create /g.bin
write /g.bin 3000 100 1
write /g.bin 100 50 2
truncate /g.bin 2500
write /g.bin 2600 20 3
truncate /g.bin 100000
write /g.bin 4090 20 4
truncate /g.bin 511
write /g.bin 0 5000 5
truncate /g.bin 1025
mkdir /gd
create /gd/h.bin
write /gd/h.bin 7000 3 6
write /gd/h.bin 1500 1500 7
OPS
ntfs_ops gaps_and_shrinks "$W/s9" || rc=1
# --- 同じ操作列を USB Mass Storage (模擬 BOT) 越しに: BlockDevice → MscDevice (WRITE/SYNC) → fs/ntfs の全経路。結果は同じ独立検証 (ntfsfix + リファレンスとのツリー比較)
export USBMEM_VIA_MSC=1
for sn in "create_mkdir:s1" "delete_recreate:s2" "rename:s3" "many_files:s4" "unicode_names:s5" "truncate:s8"; do
  ntfs_ops "msc_${sn%%:*}" "$W/${sn##*:}" || rc=1
done
unset USBMEM_VIA_MSC
# --- 小さなクラスタ (512B / 2048B: 1 folio に複数クラスタが入り、データ・未初期化・穴が混在する) のボリュームでも、同じテスト一式が通ること
if [ -z "${USBMEM_WT_NESTED:-}" ]; then
  for c in 512 2048; do
    CD=$W/fix$c
    USBMEM_NTFS_CLUSTER=$c "$HERE/gen_ntfs_fixture.sh" "$CD" >/dev/null 2>&1 || { echo "FAIL[cluster $c] fixture generation"; rc=1; continue; }
    USBMEM_WT_NESTED=1 sh "$HERE/run_write_tests.sh" "$T" "$CD" >"$W/nested.out" 2>&1 || { echo "FAIL[cluster $c]:"; grep FAIL "$W/nested.out" | head -5 | cut -c1-200; rc=1; }
    rm -rf "$CD"
  done
fi
# --- 書き込み後に manifest が (書いたファイル以外) 全て保たれること
cp "$IMG" "$W/w.img"; USBMEM_WRITE_ITER=1 "$T" --wtest "$W/w.img" big.bin 500000 20000 9 2>&1 | grep -v unimplemented | grep -v "wtest ok" || true
sed '/ big.bin$/d' "$D/ntfs_basic.manifest" > "$W/m2.manifest"
"$T" --manifest "$W/w.img" "$W/m2.manifest" 2>&1 | grep -E "manifest:" | grep -q " 0 failures" || { echo "FAIL manifest after write"; rc=1; }
echo "write tests rc=$rc"
exit $rc
