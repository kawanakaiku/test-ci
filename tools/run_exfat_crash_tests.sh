#!/bin/sh
# exFAT 書き込みの電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/exfat-write.md)。tools/run_ext2_crash_tests.sh と同じ方式:
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファして電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。
# 検査: (B) ブートセクタ (先頭 1 セクタ) 以外のメディアが 1 バイトでも変わっているなら、ボリュームは「汚れている」(VolumeDirty が立っている) = 最初の変更の前にダーティ印が確定している。
#       (A) fsck.exfat -y で直したあと、元からあった全ファイル (操作列が触らないもの) のツリー (種別・サイズ・sha256) が元と一致する。消した元ファイルは「消えている」か「元のまま」。
#       (C) fsck.exfat -y のあと fsck.exfat -n が clean。
#       (D) 修復前でも fsck.exfat -n が 0 (torn=1 はチェックサム不一致 1 件だけ許す) (書き込み順序の検査。ダーティ印・未参照クラスタ以外の不整合が出ない)。
# 元のボリュームは**独立実装** (exfat-fuse) で作る (自前ドライバで作らない)。ボリュームは小さく (クラスタ 256 個ほど) して、操作列の途中で割り当てが一周し、解放済みのクラスタが再利用される状況にする。
# 使い方: run_exfat_crash_tests.sh <usbmem_exfat_write_tests> [作業 dir (未使用: 互換のため)] [サンプル数 (既定 40)] [クラスタサイズ (既定 4096)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NS=${3:-40}; CS=${4:-4096}
for c in mkfs.exfat fsck.exfat mount.exfat-fuse fusermount3 python3 gcc cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_xfc.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
gcc -shared -fPIC -O1 -o "$W/shim.so" "$HERE/exfat_fuse_shim.c" -ldl || { echo "skip: cannot build the fusermount3 shim" >&2; exit 77; }
fmount() { mkdir -p "$2"; LD_PRELOAD="$W/shim.so" mount.exfat-fuse -o "$3" "$1" "$2" 2>/dev/null; }
# 元のボリューム: 独立実装で作る
python3 -I - "$W/pop.ops" "$W/ops" <<'PY'
import sys
P = ["mkdir /od"]
for i in range(1, 9): P += ["create /orig%d.bin" % i, "write /orig%d.bin 0 %d %d" % (i, [3000, 9000, 20000, 500, 6000, 12000, 4096, 4097][i - 1], 40 + i)]
for i in range(1, 6): P += ["create /od/o%d.bin" % i, "write /od/o%d.bin 0 %d %d" % (i, 7000 + 1000 * i, 60 + i)]
open(sys.argv[1], "w").write("\n".join(P) + "\n")
L = ["unlink /orig3.bin", "unlink /od/o2.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 14000, 90 + i)]   # 解放されたクラスタが実際に再利用される (ボリュームが小さい)
for i in range(1, 25): L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 12000, 30000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 24, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["create /cr/big.bin", "write /cr/big.bin 0 400000 3", "write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "truncate /cr/f7.bin 10", "create /top.txt", "write /top.txt 0 700 5", "rename /cr/f9.bin /cr/f3.bin", "unlink /orig5.bin", "create /late.bin", "write /late.bin 0 30000 9"]
open(sys.argv[2], "w").write("\n".join(L) + "\n")
PY
mkimg() {  # mkimg <out>: 独立実装 (exfat-fuse) で元のボリュームを作る
  rm -f "$1"; truncate -s 3M "$1"; mkfs.exfat -q -c $CS "$1"
  fmount "$1" "$W/mnt_p" rw || return 1
  python3 -I "$HERE/ops_ref.py" "$W/mnt_p" "$W/pop.ops"; fusermount3 -u "$W/mnt_p"
}
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0
tree_of() {  # tree_of <img> <out>
  fmount "$1" "$W/mnt_t" ro || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" > "$2"; fusermount3 -u "$W/mnt_t"
}
# 操作列が触る場所と、消す元ファイル (orig3, o2, orig5) を除いた「元からあるもの」のダンプ
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|late\.bin|orig3\.bin|orig5\.bin|od/o2\.bin)(/|$)' "$1" || true; }
deleted_part() { grep -E ' (orig3\.bin|orig5\.bin|od/o2\.bin)$' "$1" || true; }
flags_of() { python3 -I -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(106); print(struct.unpack('<H',f.read(2))[0])" "$1"; }
mkimg "$W/orig.img" || { echo "skip: cannot build the original volume with exfat-fuse" >&2; exit 77; }
fsck.exfat -n "$W/orig.img" >/dev/null 2>&1 || { echo "skip: original volume not clean" >&2; exit 77; }
tree_of "$W/orig.img" "$W/orig.tree" || { echo "exfat-fuse ro failed on the original" >&2; exit 77; }
orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
[ -z "${XFC_KEEP:-}" ] || cp "$W/orig.img" "$W/ops" "$XFC_KEEP/"
sum0=$(sha256sum "$W/orig.img" | cut -d" " -f1)
rc=0; n=0; inconsistent=0; a_fail=0; b_fail=0; c_fail=0; d_fail=0; dirty_after=0; clean_after=0; fsck_fixed=0; fsck_abort=0
for sync_variant in batch each; do
if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
cp "$W/orig.img" "$W/c.img"
USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" > "$W/base.log" 2>&1 || true
total=$(grep -a "^writes:" "$W/base.log" | sed 's/writes: //')
echo "操作列の総書き込み数: $total (cluster $CS, sync=$sync_variant); $(grep -a '^ops:' "$W/base.log")"
# 小さいボリュームなので割り当てが一周するはず: 操作が ENOSPC で失敗しすぎていないか (失敗しても検査は有効だが、再利用を試す機会が減る)
grep -a '^op failed' "$W/base.log" | head -3 | sed 's/^/  /' || true
[ -n "$total" ] && [ "$total" -gt 20 ] || { echo "FAIL: 総書き込み数が取れない"; exit 1; }
for mode in prefix cache; do
  i=0
  while [ $i -lt $NS ]; do
    # 序盤を密に、後半を粗くサンプリング
    if [ $i -lt 15 ]; then at=$i; else at=$(( total * (i - 14) / (NS - 14) )); fi
    for torn in 0 1; do
      cp "$W/orig.img" "$W/c.img"
      USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" >/dev/null 2>&1 || true
      n=$((n+1))
      # (B) ブートセクタ (先頭 512 バイト) 以外が変わっているなら、VolumeDirty が立っている
      chg=$(cmp -l "$W/orig.img" "$W/c.img" | awk '$1 > 512' | wc -l)
      fl=$(flags_of "$W/c.img")
      if [ "$chg" -gt 0 ] && [ $((fl & 2)) -eq 0 ]; then b_fail=$((b_fail+1)); echo "B-FAIL: sync=$sync_variant mode=$mode at=$at torn=$torn: ${chg} bytes changed outside the boot sector but VolumeDirty is not set"; fi
      if [ $((fl & 2)) -ne 0 ]; then dirty_after=$((dirty_after+1)); else clean_after=$((clean_after+1)); fi
      # (C)(A) fsck.exfat -y で直す → clean → 元のツリーが同じ
      fsck.exfat -n "$W/c.img" >/dev/null 2>&1 && r=0 || r=$?
      # (D) torn=0 (書き込み単位がセクタ以上で欠けない) なら、電源断のどの時点でも、修復前の fsck.exfat -n が 0 (ダーティ印と未参照クラスタ以外の不整合なし)。
      #     書き込み順序 (データ → FAT・ビットマップ → 新クラスタ → エントリ) を守っていないと、エントリだけが先に届いた状態が現れて不整合になる。torn=1 は 1 つのエントリセットがセクタ境界をまたぐ書き込みの途中切れがあり得るので対象外。
      #     torn=1 は複数セクタにまたがる構造 (セクタ境界をまたぐエントリセット / ルートの FAT チェーンの付け替え) の書き込みが途中で切れた場合の「ファイルのチェックサム不一致」「ルートのクラスタチェーン破損」だけはあり得る (exFAT 自体に複数セクタ更新の原子性が無い。fsck が直す)。それ以外 (ディレクトリ内の古いクラスタの残骸 = 未初期化の新クラスタが先に参照された等) は不可。
      if [ $r -ne 0 ]; then
        bad=$(fsck.exfat -n "$W/c.img" 2>&1 | grep -a "ERROR" | { if [ $torn -eq 0 ]; then cat; else grep -a -v -E "checksum .* of a file is wrong|cluster chain of root is broken" || true; fi; } | head -1)
        if [ -n "$bad" ]; then d_fail=$((d_fail+1)); echo "D-FAIL: 修復前に不整合 (sync=$sync_variant mode=$mode at=$at torn=$torn): $bad"; fi
      fi
      if [ $r -ne 0 ]; then inconsistent=$((inconsistent+1)); [ -z "${XFC_VERBOSE:-}" ] || { echo "  inconsistent(sync=$sync_variant mode=$mode at=$at torn=$torn):"; fsck.exfat -n "$W/c.img" 2>&1 | grep -v "^exfatprogs" | head -6 | sed "s/^/    /"; }; fi
      fsck.exfat -y "$W/c.img" >"$W/fsck.log" 2>&1 && r=0 || r=$?
      if [ $r -ge 2 ]; then fsck_abort=$((fsck_abort+1)); echo "C-FAIL: fsck.exfat -y exit $r (sync=$sync_variant mode=$mode at=$at torn=$torn)"; c_fail=$((c_fail+1)); continue; fi
      [ $r -ne 0 ] && fsck_fixed=$((fsck_fixed+1))
      if ! fsck.exfat -n "$W/c.img" >/dev/null 2>&1; then c_fail=$((c_fail+1)); echo "C-FAIL: not clean after fsck.exfat -y (sync=$sync_variant mode=$mode at=$at torn=$torn)"; continue; fi
      if tree_of "$W/c.img" "$W/c.tree"; then
        orig_part "$W/c.tree" > "$W/c.part"
        deleted_part "$W/c.tree" > "$W/c.del"
        if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: 消した元ファイルが別の内容で残っている (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
        if ! cmp -s "$W/orig.part" "$W/c.part"; then a_fail=$((a_fail+1)); echo "A-FAIL: 元のファイルが変わった/消えた (sync=$sync_variant mode=$mode at=$at torn=$torn)"; diff "$W/orig.part" "$W/c.part" | head -3; fi
      else a_fail=$((a_fail+1)); echo "A-FAIL: fsck 後に ro マウントできない (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
    done
    i=$((i+1))
  done
done
done
echo "試行 $n 回: (A) 元ファイル破損 $a_fail / (B) ダーティ印なしで変更 $b_fail / (C) fsck 後に clean にならない $c_fail (fsck.exfat 異常終了 $fsck_abort) / (D) 修復前に不整合 $d_fail"
echo "電源断後の状態: 汚れている(要 fsck) $dirty_after / clean のまま $clean_after; fsck 前に不整合 (fsck.exfat -n が非 0) があった試行 $inconsistent; fsck.exfat -y が修復した試行 $fsck_fixed"
[ "$(sha256sum "$W/orig.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: 元のボリューム本体が変更された"; rc=1; }
[ "$a_fail" -eq 0 ] && [ "$b_fail" -eq 0 ] && [ "$c_fail" -eq 0 ] && [ "$d_fail" -eq 0 ] || rc=1
exit $rc
