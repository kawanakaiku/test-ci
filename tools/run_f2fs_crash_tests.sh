#!/bin/sh
# F2FS 書き込み (チェックポイント) の電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/f2fs-write.md)。
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファし、電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。同期方式: 最後に 1 回 (batch) / 操作ごと (each)。
# 検査 (各カットポイント。すべて「回復の処理なし」: F2FS は古い方のチェックポイントがそのまま有効に残るのが正しい):
#   (F) fsck.f2fs --dry-run -f が完全に clean。
#   (N) 自前ドライバで書き込み可マウント → 何もせずアンマウントしてもイメージは 1 バイトも変わらない (回復で書くものが無い)。
#   (S) 操作ごとに同期する場合: ツリーが「同期が完了した操作まで」か「その次の操作まで」のどちらかの状態と完全に一致 (途中の状態が見えない = チェックポイントの原子性)。
#       最後に 1 回同期する場合: 全か無か (最初の状態か最後の状態) のどちらか。
#   (A) 元からあるファイル (操作列が触らないもの) は元のまま、消す元ファイルは「消えた」か「元のまま」(解放したブロックをコミット前に再利用すると、ここで壊れる)。
#   (K) 一部のカットポイントは本物のカーネルが読み取り専用でマウントしたツリーも自前ドライバのツリーと一致 (カーネルが使えるとき。USBMEM_F2FS_KERNEL=off で省略)。
# 使い方: run_f2fs_crash_tests.sh <usbmem_f2fs_write_tests> [サンプル数 (既定 12。0 = 書き込みを 1 回ずつ全部)。サンプルには CP パックの書き込みの前後も必ず含める] [mkfs -O]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NS=${2:-12}; FEAT=${3:-extra_attr,inode_checksum,sb_checksum,inode_crtime,lost_found}
for c in mkfs.f2fs fsck.f2fs sload.f2fs python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_f2c.XXXXXX)
trap 'rm -rf "$W"' EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
KMODE=${USBMEM_F2FS_KERNEL:-auto}
if [ "$KMODE" = auto ]; then KMODE=off; if python3 -I "$HERE/f2fs_vm/kcheck.py" --probe >/dev/null 2>&1; then KMODE=vm; fi; fi
[ "$KMODE" = loop ] && KMODE=vm   # 電源断後のイメージはループマウントではなく VM で読ませる (sudo を使わない)

# 元のボリューム: 元からあるファイルを sload.f2fs で入れる。小さな (128M) ボリューム。
python3 -I - "$W/tree" <<'PY'
import os, sys
root = sys.argv[1]
os.makedirs(os.path.join(root, "keep/sub"))
def put(p, n, s): open(os.path.join(root, p), "wb").write(bytes(((i * 7 + s) & 255) for i in range(n)))
put("direct_max.bin", 12 * 4096, 3); put("one_block.bin", 4096, 4); put("keep/a.txt", 5000, 5); put("keep/sub/b.bin", 70000, 6); put("keep/c.bin", 123, 7)
put("victim.bin", 30000, 8); put("tiny.txt", 40, 9)
put("victim2.bin", 6 * 1024 * 1024, 10)     # 数セグメント分: 消したあとの空きセグメントをコミット前に再利用すると、まだ消えていないはずの中身が壊れる
put("keep/filler.bin", 9 * 1024 * 1024, 11)
PY
rm -f "$W/base.img"; truncate -s 128M "$W/base.img"
mkfs.f2fs -q -f -O "$FEAT" "$W/base.img" >/dev/null 2>&1
sload.f2fs -f "$W/tree" "$W/base.img" > "$W/sload.log" 2>&1 || { tail -5 "$W/sload.log"; echo "sload.f2fs failed"; exit 1; }
nfsck=0
fsck_clean() { nfsck=$((nfsck + 1)); fsck.f2fs --dry-run -f "$1" > "$W/fsck.log" 2>&1 && ! grep -q '\[Fail\|\[ASSERT\]\|\[FIX\]' "$W/fsck.log"; }
fsck_clean "$W/base.img" || { cat "$W/fsck.log" | tail -5; echo "base image not clean"; exit 1; }
python3 -I - "$W/ops" <<'PY'
import sys
L = ["unlink /direct_max.bin", "unlink /one_block.bin", "unlink /victim2.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 14000, 90 + i)]
for i in range(1, 25):
    L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 5000, 70000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 20, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "symlink /cr/lnk /cr/f3.bin", "link /cr/hard /cr/f5.bin", "truncate /cr/f7.bin 10",
      "truncate /victim.bin 100", "write /victim.bin 50 9000 44", "create /top.txt", "write /top.txt 0 700 5", "write /tiny.txt 3 10 6", "create /cr/tiny2", "write /cr/tiny2 0 50 8",
      "create /cr/big.bin", "write /cr/big.bin 0 7000000 61", "create /cr/big2.bin", "write /cr/big2.bin 0 3000000 62"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
dumptree() { "$T" --dump "$1" "$2"; }
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|direct_max\.bin|one_block\.bin|victim\.bin|victim2\.bin|tiny\.txt)(/|$)' "$1" || true; }
deleted_part() { grep -E ' (direct_max\.bin|one_block\.bin|victim2\.bin)$' "$1" || true; }
dumptree "$W/base.img" "$W/orig.tree"
orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
sum0=$(sha256sum "$W/base.img" | cut -d" " -f1)
# 各操作の後の状態 (クラッシュなし実行)
mkdir -p "$W/st"; cp "$W/base.img" "$W/clean.img"
USBMEM_SYNC_EVERY=1 USBMEM_DUMP_DIR="$W/st" "$T" --ops "$W/clean.img" "$W/ops" >/dev/null 2>&1 || { echo "clean run failed"; exit 1; }
fsck_clean "$W/clean.img" || { echo "clean run: fsck.f2fs not clean"; exit 1; }
nops=$(ls "$W/st" | wc -l); nops=$((nops - 1))
n=0; f_fail=0; n_fail=0; s_fail=0; a_fail=0; k_fail=0; in_states=0; kern=0; old_cp=0; new_cp=0
last=$(printf '%04d' $nops)
for sync_variant in batch each; do
  if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
  cp "$W/base.img" "$W/c.img"
  total=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
  echo "操作列の総書き込み数: $total (${FEAT}, sync=$sync_variant, $nops ops)"
  if [ "$NS" -eq 0 ]; then ATS=$(seq 0 "$total")
  else
    cpr=$(od -An -tu4 -j$((1024 + 76)) -N4 "$W/base.img" | tr -d ' ')
    cp "$W/base.img" "$W/c.img"; USBMEM_WRITE_LOG=1 USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2> "$W/wlog" >/dev/null || true
    ATS=$(python3 -I "$HERE/f2fs_cutpoints.py" "$W/wlog" "$cpr" "$NS" "$total")
  fi
  for mode in prefix cache; do
    i=0
    for at in $ATS; do
      i=$((i+1))
      for torn in 0 1; do
        cp "$W/base.img" "$W/c.img"
        acked=$(USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a '^acked:' | sed 's/acked: //' || true)
        [ -n "$acked" ] || acked=0
        n=$((n+1)); tag="sync=$sync_variant mode=$mode at=$at torn=$torn acked=$acked"
        # (F)
        if ! fsck_clean "$W/c.img"; then f_fail=$((f_fail+1)); echo "F-FAIL: $tag: fsck.f2fs not clean"; grep '\[Fail\|\[ASSERT\]\|\[FIX\]' "$W/fsck.log" | head -4; continue; fi
        # (N)
        s1=$(sha256sum "$W/c.img" | cut -d" " -f1)
        "$T" --replay "$W/c.img" >/dev/null 2>&1 || { n_fail=$((n_fail+1)); echo "N-FAIL: $tag: mount_rw failed"; continue; }
        [ "$(sha256sum "$W/c.img" | cut -d" " -f1)" = "$s1" ] || { n_fail=$((n_fail+1)); echo "N-FAIL: $tag: mount_rw changed the image"; }
        # (S) / (A)
        dumptree "$W/c.img" "$W/c.tree" || { n_fail=$((n_fail+1)); echo "N-FAIL: $tag: cannot read the image"; continue; }
        k_hit=-1
        for k in $(seq 0 $nops); do if cmp -s "$W/c.tree" "$W/st/$(printf '%04d' $k).tree"; then k_hit=$k; break; fi; done
        if [ $sync_variant = each ]; then
          if [ $k_hit -ne $acked ] && [ $k_hit -ne $((acked + 1)) ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: state matches op $k_hit (expected $acked or $((acked+1)))"; fi
        else
          if [ $k_hit -ne 0 ] && [ $k_hit -ne $nops ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: batch state matches op $k_hit (expected 0 or $nops)"; fi
        fi
        if [ $k_hit -eq 0 ]; then old_cp=$((old_cp+1)); elif [ $k_hit -ge 0 ]; then new_cp=$((new_cp+1)); fi
        [ $k_hit -ge 0 ] && in_states=$((in_states+1))
        orig_part "$W/c.tree" > "$W/c.part"; deleted_part "$W/c.tree" > "$W/c.del"
        if ! cmp -s "$W/orig.part" "$W/c.part"; then a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 元のファイルが変わった/消えた"; diff "$W/orig.part" "$W/c.part" | head -3; fi
        if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 消した元ファイルが別の内容で残っている"; fi
        # (K) 一部だけカーネルに読ませる
        if [ "$KMODE" != off ] && [ $((n % 17)) -eq 3 ]; then
          cp "$W/c.img" "$W/k.img"
          if python3 -I "$HERE/f2fs_vm/kcheck.py" --ro "$W/k.img" "$W/k.tree" >"$W/k.log" 2>&1; then
            kern=$((kern+1)); cmp -s "$W/c.tree" "$W/k.tree" || { k_fail=$((k_fail+1)); echo "K-FAIL: $tag: kernel tree differs"; diff "$W/c.tree" "$W/k.tree" | head -3; }
          else k_fail=$((k_fail+1)); echo "K-FAIL: $tag: kernel could not mount"; tail -3 "$W/k.log"; fi
        fi
      done
    done
  done
done
echo "試行 $n 回 (カットポイント): (F) fsck.f2fs が clean でない $f_fail / (N) 回復で書き換え・読めない $n_fail / (S) 原子性 $s_fail / (A) 元ファイル破損 $a_fail / (K) カーネルの不一致 $k_fail"
echo "電源断後: 古いチェックポイントのまま $old_cp / 新しいチェックポイント (いずれかの操作境界) $new_cp / 状態の一致 $in_states; fsck.f2fs 検査 $nfsck 回; カーネルが読んだ $kern 回 ($KMODE)"
rc=0
[ "$(sha256sum "$W/base.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: base image changed"; rc=1; }
[ $f_fail -eq 0 ] && [ $n_fail -eq 0 ] && [ $s_fail -eq 0 ] && [ $a_fail -eq 0 ] && [ $k_fail -eq 0 ] || rc=1
exit $rc
