#!/bin/sh
# Btrfs 書き込みの電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/btrfs-write.md)。tools/run_ext4_crash_tests.sh と同じ方式:
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファし、電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。同期方式: 最後に 1 回 (batch) / 操作ごと (each)。
# Btrfs にはジャーナルの再生が無く、電源断後のイメージは「そのまま」整合していなければならない (COW + スーパーブロック最後)。検査 (各カットポイント):
#   (K) 有効なスーパーブロックのコピー (本体 / 64 MiB のミラー) のそれぞれについて、btrfs check --readonly (-s N) が完全に clean。
#   (S) btrfs restore で取り出したツリーが「いずれかの操作の境界の状態」(自前ドライバの電源断なし実行が各操作の後に書いたダンプ) と完全に一致。
#       操作ごとに同期する場合は、完了した操作の数 acked か acked+1 のどちらか (途中の状態が見えない = 原子性)。
#   (A) 元からあるファイル (操作列が触らないもの) が元のまま。消す元ファイルは「消えた」か「元のまま」(別の内容が混ざらない = 解放ブロックを同じ世代で再利用していない)。
#   (P) 続き: 書き込み可でマウントできるなら (できないのはミラーの世代不一致 = 書き込み拒否の想定内) 追加の操作をして同期 → btrfs check が clean、元のファイルは元のまま。
# 使い方: run_btrfs_crash_tests.sh <usbmem_btrfs_write_tests> [サンプル数 (既定 14)] [mkfs.btrfs の追加オプション...]   イメージの大きさは環境変数 USBMEM_CRASH_IMG_SIZE (既定 160M。-m dup -d dup なら 320M 以上)
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NS=${2:-14}; shift 2 2>/dev/null || shift $#
MKOPT="$*"
for c in mkfs.btrfs btrfs python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_btrfsc.XXXXXX)
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
python3 -I - "$W/tree" <<'PY'
import os, sys
root = sys.argv[1]
os.makedirs(os.path.join(root, "keep/sub"))
def put(p, n, s): open(os.path.join(root, p), "wb").write(bytes(((i * 7 + s) & 255) for i in range(n)))
put("tiny.txt", 300, 3); put("one_sector.bin", 4096, 4); put("keep/a.txt", 5000, 5); put("keep/sub/b.bin", 70000, 6); put("keep/c.bin", 123, 7)
put("victim.bin", 300000, 8); put("victim2.bin", 900000, 9); put("direct.bin", 20000, 10)
os.symlink("keep/a.txt", os.path.join(root, "keep/ln"))
PY
rm -f "$W/base.img"; truncate -s "${USBMEM_CRASH_IMG_SIZE:-160M}" "$W/base.img"
# shellcheck disable=SC2086
mkfs.btrfs -q -f --rootdir "$W/tree" $MKOPT "$W/base.img" >/dev/null
btrfs check --readonly "$W/base.img" >/dev/null 2>&1 || { echo "base image not clean"; exit 1; }
python3 -I - "$W/ops" <<'PY'
import sys
L = ["unlink /victim.bin", "unlink /victim2.bin", "unlink /direct.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 140000, 90 + i)]   # 消した元ファイルの跡 (低い番地) に確保される大きさ
for i in range(1, 25):
    L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 5000, 70000, 300][i % 4], i)]
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 20, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "symlink /cr/lnk /cr/f3.bin", "link /cr/hard /cr/f5.bin", "truncate /cr/f7.bin 10",
      "write /tiny.txt 100 5000 44", "truncate /one_sector.bin 100", "create /top.txt", "write /top.txt 0 700 5",
      "create /cr/big.bin", "write /cr/big.bin 0 1400000 61", "write /keep/a.txt 10 20 3", "unlink /keep/c.bin"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
tree_of() {  # tree_of <img> <out>: btrfs restore (独立実装) で取り出したツリー
  d=$(mktemp -d "$W/rs.XXXXXX")
  btrfs restore -S -i "$1" "$d" >"$W/restore.log" 2>&1 || { rm -rf "$d"; return 1; }
  python3 -I "$HERE/ops_ref.py" --tree "$d" > "$2"; rm -rf "$d"
}
# 操作列が触る場所と、ここで変更・削除する元ファイルを除いた「元からあるもの」
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|victim\.bin|victim2\.bin|direct\.bin|post|tiny\.txt|one_sector\.bin|keep/a\.txt|keep/c\.bin)(/|$)' "$1" || true; }
deleted_part() { grep -E ' (victim\.bin|victim2\.bin|direct\.bin)$' "$1" || true; }
tree_of "$W/base.img" "$W/orig.tree" || { echo "restore failed on base"; exit 1; }
orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
sum0=$(sha256sum "$W/base.img" | cut -d" " -f1)
# 各操作の後の状態 (電源断なし)。最終状態は POSIX 操作の参照 (btrfs restore したツリーに同じ操作列を実行) と一致することを確認する
mkdir -p "$W/st"; cp "$W/base.img" "$W/clean.img"
USBMEM_SYNC_EVERY=1 USBMEM_DUMP_DIR="$W/st" "$T" --ops "$W/clean.img" "$W/ops" >/dev/null 2>&1 || { echo "clean run failed"; exit 1; }
btrfs check --readonly --check-data-csum "$W/clean.img" >/dev/null 2>&1 || { echo "clean run: btrfs check not clean"; exit 1; }
d=$W/refd; mkdir -p "$d"; btrfs restore -S -i "$W/base.img" "$d" >/dev/null 2>&1; python3 -I "$HERE/ops_ref.py" "$d" "$W/ops"; python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/ref.tree"; rm -rf "$d"
nops=$(ls "$W/st" | wc -l); nops=$((nops - 1)); last=$(printf '%04d' $nops)
cmp -s "$W/st/$last.tree" "$W/ref.tree" || { diff "$W/st/$last.tree" "$W/ref.tree" | head; echo "final state differs from the POSIX reference"; exit 1; }
n=0; k_fail=0; s_fail=0; a_fail=0; p_fail=0; nfsck=2; in_states=0; changed=0; same_as_base=0; post_done=0; post_ro=0; copies=0
sb_ok() {  # sb_ok <img> <N>: N 番目のスーパーブロックのコピーのチェックサムが正しいか
  btrfs inspect-internal dump-super -s "$2" "$1" 2>/dev/null | grep -q "^csum.*\[match\]"
}
for sync_variant in batch each; do
  if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
  cp "$W/base.img" "$W/c.img"
  total=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
  echo "操作列の総書き込み数: $total (sync=$sync_variant, $nops ops, mkfs: ${MKOPT:-既定})"
  for mode in prefix cache; do
    i=0
    while [ $i -lt "$NS" ]; do
      if [ $i -lt 8 ]; then at=$i; else at=$(( total * (i - 7) / (NS - 7) )); fi
      for torn in 0 1; do
        cp "$W/base.img" "$W/c.img"
        acked=$(USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a '^acked:' | sed 's/acked: //' || true)
        [ -n "$acked" ] || acked=0
        n=$((n+1)); tag="sync=$sync_variant mode=$mode at=$at torn=$torn acked=$acked"
        cmp -s "$W/base.img" "$W/c.img" && same_as_base=$((same_as_base+1)) || changed=$((changed+1))
        # (K) 有効なスーパーブロックのコピーごとに btrfs check
        valid=0
        for sbn in 0 1; do
          if sb_ok "$W/c.img" $sbn; then
            valid=$((valid+1)); copies=$((copies+1)); nfsck=$((nfsck+1))
            if ! btrfs check --readonly --check-data-csum -s $sbn "$W/c.img" > "$W/fsck.log" 2>&1; then
              k_fail=$((k_fail+1)); echo "K-FAIL: $tag: btrfs check -s $sbn not clean"; grep -v "^\[\|^Opening\|^Checking" "$W/fsck.log" | head -6
            fi
          fi
        done
        [ $valid -gt 0 ] || { k_fail=$((k_fail+1)); echo "K-FAIL: $tag: no valid superblock copy"; continue; }
        # (S)(A)
        if ! tree_of "$W/c.img" "$W/c.tree"; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: btrfs restore failed"; continue; fi
        k_hit=-1
        for k in $(seq 0 $nops); do if cmp -s "$W/c.tree" "$W/st/$(printf '%04d' $k).tree"; then k_hit=$k; break; fi; done
        if [ $k_hit -lt 0 ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: tree matches no operation boundary state"; diff "$W/c.tree" "$W/st/0000.tree" | head -4
        else
          in_states=$((in_states+1))
          if [ $sync_variant = each ] && [ $k_hit -ne "$acked" ] && [ $k_hit -ne $((acked + 1)) ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: state matches op $k_hit (expected $acked or $((acked+1)))"; fi
        fi
        orig_part "$W/c.tree" > "$W/c.part"; deleted_part "$W/c.tree" > "$W/c.del"
        cmp -s "$W/orig.part" "$W/c.part" || { a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 元のファイルが変わった/消えた"; diff "$W/orig.part" "$W/c.part" | head -3; }
        if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 消した元ファイルが別の内容で残っている"; fi
        # (P) 続きの書き込み
        cp "$W/c.img" "$W/p.img"
        pout=$("$T" --post "$W/p.img" 2>&1) || { p_fail=$((p_fail+1)); echo "P-FAIL: $tag: post driver failed: $pout" | head -3; continue; }
        case "$pout" in
          *"post: done"*)
            post_done=$((post_done+1)); nfsck=$((nfsck+1))
            btrfs check --readonly --check-data-csum "$W/p.img" > "$W/fsck.log" 2>&1 || { p_fail=$((p_fail+1)); echo "P-FAIL: $tag: btrfs check not clean after continuing"; grep -v "^\[\|^Opening\|^Checking" "$W/fsck.log" | head -6; continue; }
            tree_of "$W/p.img" "$W/p.tree" && orig_part "$W/p.tree" > "$W/p.part" && cmp -s "$W/orig.part" "$W/p.part" || { p_fail=$((p_fail+1)); echo "P-FAIL: $tag: original files changed after continuing"; } ;;
          *) post_ro=$((post_ro+1)) ;;
        esac
      done
      i=$((i+1))
    done
  done
done
echo "試行 $n 回 (カットポイント): (K) clean でない $k_fail / (S) 操作境界の状態でない・原子性違反 $s_fail / (A) 元ファイル破損 $a_fail / (P) 続きの書き込み後に異常 $p_fail"
echo "電源断後: 変更なし $same_as_base / 変更あり $changed / いずれかの操作境界の状態と一致 $in_states / 続きを書けた $post_done / 書き込み拒否 (ミラー不一致など) $post_ro; 有効なスーパーブロックのコピー $copies 個を検査; btrfs check 計 $nfsck 回"
rc=0
[ "$(sha256sum "$W/base.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: base image changed"; rc=1; }
[ $k_fail -eq 0 ] && [ $s_fail -eq 0 ] && [ $a_fail -eq 0 ] && [ $p_fail -eq 0 ] || rc=1
exit $rc
