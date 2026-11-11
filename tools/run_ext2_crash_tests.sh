#!/bin/sh
# ext2 書き込みの電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/ext2-write.md)。tools/run_crash_tests.sh (NTFS) と同じ方式:
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファして電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。
# 検査: (B) スーパーブロック以外のメディアが 1 バイトでも変わっているなら、ボリュームは「汚れている」(s_state が VALID_FS でない) = 最初の変更の前にダーティ印が確定している。
#       (A) e2fsck -fy で直したあと、元からあった全ファイル (操作列が触らないもの) のツリー (種別・サイズ・sha256・リンク先) が元と一致する。
#       (C) e2fsck -fy のあと e2fsck -fn が clean。
# 使い方: run_ext2_crash_tests.sh <usbmem_ext2_write_tests> [フィクスチャ dir] [サンプル数 (既定 40)] [フィクスチャ名 (既定 b1k)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_ext2_${USER:-u}}; NS=${3:-40}; FX=${4:-b1k}
for c in e2fsck fuse2fs fusermount3 python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/$FX.img" ] || { echo "skip: no fixture $D/$FX.img" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_e2c.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
python3 -I - "$W/ops" <<'PY'
import sys
L = ["unlink /direct_max.bin", "unlink /one_block.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 14000, 90 + i)]   # ルート直下 (元ファイルと同じグループ) に書く: 解放されたブロックが実際に再利用される   # 元のファイルを 2 つ消してから確保する: 解放ブロックの再利用が同期より先に起きると、電源断後に消したはずの元ファイルに新しい内容が混ざる
for i in range(1, 41):
    L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 5000, 70000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 30, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "symlink /cr/lnk /cr/f3.bin", "link /cr/hard /cr/f5.bin", "truncate /cr/f7.bin 10", "create /top.txt", "write /top.txt 0 700 5"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0
tree_of() {  # tree_of <img> <out>
  mkdir -p "$W/mnt_t"; fuse2fs -o ro,fakeroot "$1" "$W/mnt_t" 2>/dev/null || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" > "$2"; fusermount3 -u "$W/mnt_t"
}
# 操作列が触る場所 (cr, top.txt) と lost+found (fsck が孤児を入れる) を除いた「元からあるもの」のダンプ
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|direct_max\.bin|one_block\.bin)(/|$)| lost\+found(/|$)' "$1" || true; }
# 操作列が消す元ファイル: 電源断後は「消えている」か「元のまま」のどちらか (新しいファイルの内容が混ざらない)
deleted_part() { grep -E ' (direct_max\.bin|one_block\.bin)$' "$1" || true; }
state_of() { python3 -I -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(1024+58); print(struct.unpack('<H',f.read(2))[0])" "$1"; }
tree_of "$D/$FX.img" "$W/orig.tree" || { echo "fuse2fs failed on fixture" >&2; exit 77; }
orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
sum0=$(sha256sum "$D/$FX.img" | cut -d" " -f1)
rc=0; n=0; inconsistent=0; a_fail=0; b_fail=0; c_fail=0; dirty_after=0; clean_after=0; fsck_fixed=0; fsck_abort=0
for sync_variant in batch each; do
if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
cp "$D/$FX.img" "$W/c.img"
total=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
echo "操作列の総書き込み数: $total (fixture $FX, sync=$sync_variant)"
for mode in prefix cache; do
  i=0
  while [ $i -lt $NS ]; do
    # 序盤を密に、後半を粗くサンプリング
    if [ $i -lt 15 ]; then at=$i; else at=$(( total * (i - 14) / (NS - 14) )); fi
    for torn in 0 1; do
      cp "$D/$FX.img" "$W/c.img"
      USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" >/dev/null 2>&1 || true
      n=$((n+1))
      # (B) スーパーブロック (1024..2047) 以外が変わっているなら、ダーティ印が付いている
      chg=$(cmp -l "$D/$FX.img" "$W/c.img" | awk '$1 <= 1024 || $1 > 2048' | wc -l)
      st=$(state_of "$W/c.img")
      if [ "$chg" -gt 0 ] && [ $((st & 1)) -eq 1 ] && [ $((st & 2)) -eq 0 ]; then b_fail=$((b_fail+1)); echo "B-FAIL: sync=$sync_variant mode=$mode at=$at torn=$torn: ${chg} bytes changed outside the superblock but state=$st (clean)"; fi
      if [ $((st & 1)) -eq 0 ]; then dirty_after=$((dirty_after+1)); else clean_after=$((clean_after+1)); fi
      # (C)(A) e2fsck -fy で直す → clean → 元のツリーが同じ
      e2fsck -fn "$W/c.img" >/dev/null 2>&1 && r=0 || r=$?
      [ $r -ge 4 ] && inconsistent=$((inconsistent+1))   # 本当に不整合 (フラグだけの汚れは 0 か 1)
      e2fsck -fy "$W/c.img" >"$W/fsck.log" 2>&1 && r=0 || r=$?
      if [ $r -ge 8 ]; then fsck_abort=$((fsck_abort+1)); echo "C-FAIL: e2fsck -fy exit $r (sync=$sync_variant mode=$mode at=$at torn=$torn)"; c_fail=$((c_fail+1)); continue; fi
      [ $r -ne 0 ] && fsck_fixed=$((fsck_fixed+1))
      if ! e2fsck -fn "$W/c.img" >/dev/null 2>&1; then c_fail=$((c_fail+1)); echo "C-FAIL: not clean after e2fsck -fy (sync=$sync_variant mode=$mode at=$at torn=$torn)"; continue; fi
      if tree_of "$W/c.img" "$W/c.tree"; then
        orig_part "$W/c.tree" > "$W/c.part"
        # 消した元ファイルが残っているなら、中身が元のまま (行が元のダンプのどちらかと完全一致)
        deleted_part "$W/c.tree" > "$W/c.del"
        if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: 消した元ファイルが別の内容で残っている (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
        if ! cmp -s "$W/orig.part" "$W/c.part"; then a_fail=$((a_fail+1)); echo "A-FAIL: 元のファイルが変わった/消えた (sync=$sync_variant mode=$mode at=$at torn=$torn)"; diff "$W/orig.part" "$W/c.part" | head -3; fi
      else a_fail=$((a_fail+1)); echo "A-FAIL: fsck 後に ro マウントできない (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
    done
    i=$((i+1))
  done
done
done
echo "試行 $n 回: (A) 元ファイル破損 $a_fail / (B) ダーティ印なしで変更 $b_fail / (C) fsck 後に clean にならない $c_fail (e2fsck 異常終了 $fsck_abort)"
echo "電源断後の状態: 汚れている(要 fsck) $dirty_after / clean のまま $clean_after; fsck 前に実際の不整合 (e2fsck -fn が 4 以上) があった試行 $inconsistent; e2fsck -fy が非 0 で終了した試行 $fsck_fixed (汚れフラグだけの場合も含む)"
[ "$(sha256sum "$D/$FX.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: フィクスチャ本体が変更された"; rc=1; }
[ "$a_fail" -eq 0 ] && [ "$b_fail" -eq 0 ] && [ "$c_fail" -eq 0 ] || rc=1
exit $rc
