#!/bin/sh
# ext4 書き込み (jbd2) の電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/ext4-write.md)。
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファし、電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。同期方式: 最後に 1 回 (batch) / 操作ごと (each)。
# 検査 (各カットポイント):
#   (R) 自前ドライバで再生 (書き込み可でマウント → アンマウント) した後、e2fsck -fn が完全に clean (修正の必要なし)。
#   (E) 独立実装の再生: 同じイメージを e2fsck -fy (e2fsprogs の jbd2 再生) で直した結果と、自前の再生結果のツリー (fuse2fs で読む) が一致。
#   (S) 操作ごとに同期する場合: ツリーが「同期が完了した操作まで」か「その次の操作まで」のどちらかの状態と完全に一致 (途中の状態が見えない = ジャーナルの原子性)。
#       最後に 1 回同期する場合: 元からあるファイル (操作列が触らないもの) は元のまま、消す元ファイルは「消えた」か「元のまま」。
#   (B) スーパーブロック以外のメディアが変わっているなら、needs_recovery (ジャーナル使用中の印) が立っている = 印が最初の変更より先に確定している。
# 使い方: run_ext4_crash_tests.sh <usbmem_ext4_write_tests> [サンプル数 (既定 12)] [ブロックサイズ (既定 1024)] [mke2fs -O]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NS=${2:-12}; BS=${3:-1024}; FEAT=${4:-}
for c in mke2fs e2fsck fuse2fs fusermount3 python3 cmp dumpe2fs; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_e4c.XXXXXX)
cleanup() { for m in "$W"/mnt*; do fusermount3 -u "$m" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
# 元のボリューム (小さくして空きブロックの再利用を起こす): 元からあるファイルを mke2fs -d で入れる
python3 -I - "$W/tree" $BS <<'PY'
import os, sys
root, bs = sys.argv[1], int(sys.argv[2])
os.makedirs(os.path.join(root, "keep/sub"))
def put(p, n, s): open(os.path.join(root, p), "wb").write(bytes(((i * 7 + s) & 255) for i in range(n)))
put("direct_max.bin", 12 * bs, 3); put("one_block.bin", bs, 4); put("keep/a.txt", 5000, 5); put("keep/sub/b.bin", 70000, 6); put("keep/c.bin", 123, 7)
put("victim.bin", 30000, 8)
PY
mkbase() { rm -f "$W/base.img"; if [ -n "$FEAT" ]; then mke2fs -q -t ext4 -b $BS -O "$FEAT" -d "$W/tree" "$W/base.img" 12M; else mke2fs -q -t ext4 -b $BS -d "$W/tree" "$W/base.img" 12M; fi; }
# ほぼ満杯にする: keep/filler.bin (残す) と victim2.bin (操作列の最初の方で消す) で空きを埋め、後で書く大きなファイル (cr/big.bin) が
# victim2.bin の解放したブロックを使わないと入らないようにする。解放したブロックをコミット前に再利用すると、電源断のあと「まだ消えていない」
# victim2.bin の中身が新しいデータに入れ替わる (A で検出)。
mkbase
free=$(dumpe2fs -h "$W/base.img" 2>/dev/null | sed -n 's/^Free blocks: *//p')
python3 -I - "$W/tree" $BS $free <<'PY'
import os, sys
root, bs, free = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def put(p, n, s): open(os.path.join(root, p), "wb").write(bytes(((i * 7 + s) & 255) for i in range(n)))
victim = 1500 * 1024
filler = free * bs - victim - 900 * 1024
put("victim2.bin", victim, 9); put("keep/filler.bin", max(filler, 0), 10)
PY
mkbase
e2fsck -fn "$W/base.img" >/dev/null 2>&1 || { echo "base image not clean"; exit 1; }
python3 -I - "$W/ops" <<'PY'
import sys
L = ["unlink /direct_max.bin", "unlink /one_block.bin", "unlink /victim2.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 14000, 90 + i)]
for i in range(1, 25):
    L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 5000, 70000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 20, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "symlink /cr/lnk /cr/f3.bin", "link /cr/hard /cr/f5.bin", "truncate /cr/f7.bin 10",
      "truncate /victim.bin 100", "write /victim.bin 50 9000 44", "create /top.txt", "write /top.txt 0 700 5",
      "create /cr/big.bin", "write /cr/big.bin 0 1400000 61"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
tree_of() {  # tree_of <img> <out>
  mkdir -p "$W/mnt_t"; fuse2fs -o ro,fakeroot "$1" "$W/mnt_t" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/mnt_t" | grep -v -E ' lost\+found(/|$)' > "$2"; fusermount3 -u "$W/mnt_t"
}
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|direct_max\.bin|one_block\.bin|victim\.bin|victim2\.bin)(/|$)' "$1" || true; }
deleted_part() { grep -E ' (direct_max\.bin|one_block\.bin|victim2\.bin)$' "$1" || true; }
recover_flag() { dumpe2fs -h "$1" 2>/dev/null | grep -c needs_recovery || true; }
tree_of "$W/base.img" "$W/orig.tree"
orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
sum0=$(sha256sum "$W/base.img" | cut -d" " -f1)
# 各操作の後の状態 (自前ドライバのクラッシュなし実行。最終状態は fuse2fs で同じ操作列を実行した結果と一致することを確認する)
mkdir -p "$W/st"; cp "$W/base.img" "$W/clean.img"
USBMEM_SYNC_EVERY=1 USBMEM_DUMP_DIR="$W/st" "$T" --ops "$W/clean.img" "$W/ops" >/dev/null 2>&1 || { echo "clean run failed"; exit 1; }
e2fsck -fn "$W/clean.img" >/dev/null 2>&1 || { echo "clean run: e2fsck not clean"; exit 1; }
cp "$W/base.img" "$W/ref.img"; mkdir -p "$W/mnt_r"; fuse2fs -o rw,fakeroot "$W/ref.img" "$W/mnt_r" >/dev/null 2>&1; python3 -I "$HERE/ops_ref.py" "$W/mnt_r" "$W/ops"; fusermount3 -u "$W/mnt_r"
tree_of "$W/ref.img" "$W/ref.tree"; nops=$(ls "$W/st" | wc -l); nops=$((nops - 1))
for f in "$W"/st/*.tree; do grep -v -E ' lost\+found(/|$)' "$f" > "$f.x"; mv "$f.x" "$f"; done
last=$(printf '%04d' $nops)
cmp -s "$W/st/$last.tree" "$W/ref.tree" || { diff "$W/st/$last.tree" "$W/ref.tree" | head; echo "final state differs from fuse2fs reference"; exit 1; }
n=0; r_fail=0; e_fail=0; s_fail=0; a_fail=0; b_fail=0; in_states=0; partial=0; clean_after=0; replayed=0; nfsck=1
for sync_variant in batch each; do
  if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
  cp "$W/base.img" "$W/c.img"
  total=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
  echo "操作列の総書き込み数: $total (bs $BS ${FEAT:+-O $FEAT }sync=$sync_variant, $nops ops)"
  for mode in prefix cache; do
    i=0
    while [ $i -lt $NS ]; do
      if [ $i -lt 6 ]; then at=$i; else at=$(( total * (i - 5) / (NS - 5) )); fi
      for torn in 0 1; do
        cp "$W/base.img" "$W/c.img"
        acked=$(USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a '^acked:' | sed 's/acked: //' || true)
        [ -n "$acked" ] || acked=0
        n=$((n+1)); tag="sync=$sync_variant mode=$mode at=$at torn=$torn acked=$acked"
        # (B) 変更があれば needs_recovery が立っている
        chg=$(cmp -l "$W/base.img" "$W/c.img" | awk '$1 <= 1024 || $1 > 2048' | wc -l)
        rf=$(recover_flag "$W/c.img")
        if [ "$chg" -gt 0 ] && [ "$rf" -eq 0 ]; then b_fail=$((b_fail+1)); echo "B-FAIL: $tag: ${chg} bytes changed but needs_recovery not set"; fi
        [ "$rf" -gt 0 ] && replayed=$((replayed+1))
        [ "$chg" -eq 0 ] && clean_after=$((clean_after+1))
        # (E) の参照: e2fsprogs の再生
        cp "$W/c.img" "$W/e.img"; e2fsck -fy "$W/e.img" >/dev/null 2>&1 || true
        # (R) 自前の再生 → e2fsck -fn が clean
        "$T" --replay "$W/c.img" >"$W/replay.log" 2>&1 || { r_fail=$((r_fail+1)); echo "R-FAIL: $tag: replay failed"; tail -3 "$W/replay.log"; continue; }
        nfsck=$((nfsck+1))
        if ! e2fsck -fn "$W/c.img" > "$W/fsck.log" 2>&1; then r_fail=$((r_fail+1)); echo "R-FAIL: $tag: not clean after our replay"; grep -v "^Pass\|^e2fsck\|^$" "$W/fsck.log" | head -4; continue; fi
        if [ "$(recover_flag "$W/c.img")" -gt 0 ]; then r_fail=$((r_fail+1)); echo "R-FAIL: $tag: needs_recovery still set after our replay"; continue; fi
        tree_of "$W/c.img" "$W/c.tree" || { r_fail=$((r_fail+1)); echo "R-FAIL: $tag: fuse2fs cannot mount"; continue; }
        tree_of "$W/e.img" "$W/e.tree" || { e_fail=$((e_fail+1)); echo "E-FAIL: $tag: fuse2fs cannot mount e2fsck result"; continue; }
        cmp -s "$W/c.tree" "$W/e.tree" || { e_fail=$((e_fail+1)); echo "E-FAIL: $tag: our replay differs from e2fsck's replay"; diff "$W/c.tree" "$W/e.tree" | head -3; }
        # (S) / (A)
        k_hit=-1
        for k in $(seq 0 $nops); do if cmp -s "$W/c.tree" "$W/st/$(printf '%04d' $k).tree"; then k_hit=$k; break; fi; done
        if [ $sync_variant = each ]; then
          if [ $k_hit -ne $acked ] && [ $k_hit -ne $((acked + 1)) ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: state matches op $k_hit (expected $acked or $((acked+1)))"; fi
        fi
        [ $k_hit -ge 0 ] && in_states=$((in_states+1)) || partial=$((partial+1))
        orig_part "$W/c.tree" > "$W/c.part"; deleted_part "$W/c.tree" > "$W/c.del"
        if ! cmp -s "$W/orig.part" "$W/c.part"; then a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 元のファイルが変わった/消えた"; diff "$W/orig.part" "$W/c.part" | head -3; fi
        if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 消した元ファイルが別の内容で残っている"; fi
      done
      i=$((i+1))
    done
  done
done
echo "試行 $n 回 (カットポイント): (R) 再生後に clean でない $r_fail / (E) e2fsck の再生と不一致 $e_fail / (S) 原子性 (操作ごと同期) $s_fail / (A) 元ファイル破損 $a_fail / (B) 印なしで変更 $b_fail"
echo "電源断後: ジャーナル再生が必要だった $replayed / 変更なし $clean_after / いずれかの操作境界の状態と一致 $in_states / 操作の途中の状態 (最後に 1 回同期の大きなトランザクション) $partial; e2fsck -fn 検査 $nfsck 回"
rc=0
[ "$(sha256sum "$W/base.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: base image changed"; rc=1; }
[ $r_fail -eq 0 ] && [ $e_fail -eq 0 ] && [ $s_fail -eq 0 ] && [ $a_fail -eq 0 ] && [ $b_fail -eq 0 ] || rc=1
exit $rc
