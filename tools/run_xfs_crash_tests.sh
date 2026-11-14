#!/bin/sh
# XFS 書き込みの電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/xfs-write.md)。tools/run_btrfs_crash_tests.sh と同じ方式:
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファし、電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。同期方式: 最後に 1 回 (batch) / 操作ごと (each)。
# XFS は自前のドライバがジャーナルに書く (カーネルが再生できる形式)。電源断後のイメージは、ログが未再生 (dirty) のまま残りうる。検査 (各カットポイント):
#   (K) 実カーネルで rw マウント (ここでログが再生される) できて、そのあと xfs_repair -n が完全に clean (tools/xfs_vm/kreplay.py。loop マウントか QEMU の VM)。
#   (S) カーネルが見るツリーが「いずれかの操作の境界の状態」(自前ドライバの電源断なし実行が各操作の後に書いたダンプ) と完全に一致。操作ごとに同期する場合は acked か acked+1 のどちらか (原子性)。
#   (A) 元からあるファイル (操作列が触らないもの) が元のまま。
#   (P) 続き: 再生後のイメージを自前ドライバで書き込みマウントして追加の操作 → xfs_repair -n が clean。標本の一部は実カーネルにも追加の書き込みをさせる (自前が書いた空き領域・inode の木をカーネルが信用して使えるか)。
# 使い方: run_xfs_crash_tests.sh <usbmem_xfs_write_tests> [stride (既定 1 = 全カットポイント。N なら N 回に 1 回)]   環境変数: USBMEM_CRASH_MODES (既定 "prefix cache")
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/xfs_common.sh"
T=$1; STRIDE=${2:-1}
for c in mkfs.xfs xfs_repair xfs_logprint python3 cmp; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
python3 -I "$HERE/xfs_vm/kreplay.py" --probe >/dev/null 2>&1 || { rc=$?; [ $rc -ne 77 ] || { echo "skip: no kernel backend" >&2; exit 77; }; }
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_xfsc.XXXXXX)
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=0}
python3 -I "$HERE/gen_xfs_ops.py" crashseed "$W/tree"
python3 -I "$HERE/gen_xfs_ops.py" crashops "$W/ops"
mk_xfs "$W/base.img" 320M "$W/tree" || { echo "mkfs failed"; exit 1; }
xfs_clean "$W/base.img" || { echo "base image not clean"; exit 1; }
tree_of_dir() { python3 -I "$HERE/ops_ref.py" --tree "$1"; }
orig_part() { grep -v -E ' (cr|post|kpost|tiny\.txt|one_block\.bin|mid\.bin|keep/a\.txt|keep/c\.bin|keep/sub/b\.bin|sfd/t[0-9]+|sfd/s00|blkd/u[0-9]+|blkd/n000|leafd/v[0-9]+|leafd/m000|nodd/(f[^/ ]*|e(0000|0017|0034|0051|0068|0085|0102|0119|0136|0153|0170|0187|0204|0221|0238|0255|0272|0289|0306|0323|0340|0357|0374|0391|0408|0425|0442|0459|0476|0493)))(/|$)' "$1" || true; }
cp -a "$W/tree" "$W/refd"
tree_of_dir "$W/refd" > "$W/orig.tree"
orig_part "$W/orig.tree" > "$W/orig.part"
sum0=$(sha256sum "$W/base.img" | cut -d" " -f1)
# 各操作の後の状態 (電源断なし)。最終状態は POSIX 操作の参照 (元のツリーに同じ操作列を実行) と一致することを確認する
mkdir -p "$W/st"; cp --sparse=always "$W/base.img" "$W/clean.img"
USBMEM_SYNC_EVERY=1 USBMEM_DUMP_DIR="$W/st" "$T" --ops "$W/clean.img" "$W/ops" >/dev/null 2>&1 || { echo "clean run failed"; exit 1; }
xfs_clean "$W/clean.img" || { echo "clean run: xfs_repair not clean"; exit 1; }
xfs_logprint_ok "$W/clean.img" || { echo "clean run: log not CLEAN"; exit 1; }
python3 -I "$HERE/ops_ref.py" "$W/refd" "$W/ops"; tree_of_dir "$W/refd" > "$W/ref.tree"
nops=$(ls "$W/st" | wc -l); nops=$((nops - 1)); last=$(printf '%04d' $nops)
cmp -s "$W/st/$last.tree" "$W/ref.tree" || { diff "$W/st/$last.tree" "$W/ref.tree" | head; echo "final state differs from the POSIX reference"; exit 1; }
# 状態の表 (sha256 → 操作番号)。既存のブロックへの上書き (write) はその場で書くので、コミット前の電源断で中身だけが先に変わって見える (ext4 の ordered と同じ。メタデータとの原子性は無い)。
# そのため上書きするファイル (tiny.txt など) は中身の sha256 を比較から外し、サイズ・存在だけを操作境界の状態と突き合わせる。
norm() { sed -E 's/^(f [0-9]+) [0-9a-f]{64} (tiny\.txt|one_block\.bin|mid\.bin|keep\/a\.txt)$/\1 - \2/' "$1" | LC_ALL=C sort; }   # sha を消すと並びが変わるので並べ直す
: > "$W/states.tbl"
for k in $(seq 0 $nops); do echo "$(norm "$W/st/$(printf '%04d' $k).tree" | sha256sum | cut -d' ' -f1) $k" >> "$W/states.tbl"; done
n=0; k_fail=0; s_fail=0; a_fail=0; p_fail=0; kp_fail=0; nfsck=2; in_states=0; changed=0; same_as_base=0; post_done=0; post_ro=0; dirty_images=0; kpost_n=0; replayed=0; logprint_bad=0
CH=${USBMEM_CRASH_CHUNK:-16}
modes=${USBMEM_CRASH_MODES:-"prefix cache"}
for sync_variant in batch each; do
  if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
  cp --sparse=always "$W/base.img" "$W/c.img"
  total_w=$(USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
  echo "操作列の総書き込み数: $total_w (sync=$sync_variant, $nops ops, stride=$STRIDE)"
  for mode in $modes; do
    # cache 方式では flush の呼び出しもカットポイントに数える (flush の直前に切れる)。総数も cache 方式で数え直す
    if [ "$mode" = cache ]; then
      export USBMEM_CRASH_FLUSH_CUTS=1; cp --sparse=always "$W/base.img" "$W/c.img"
      total=$(USBMEM_CRASH_AT=-1 USBMEM_CRASH_MODE=cache "$T" --ops "$W/c.img" "$W/ops" 2>&1 | grep -a "^writes:" | sed 's/writes: //')
      echo "  (cache 方式の総カットポイント数: $total = 書き込み + flush)"
    else unset USBMEM_CRASH_FLUSH_CUTS || true; total=$total_w; fi
    at=${USBMEM_CRASH_FROM:-0}; [ -z "${USBMEM_CRASH_TO:-}" ] || total=$USBMEM_CRASH_TO
    while [ $at -le "$total" ]; do
      # チャンク: at から CH 個のカットポイント (× torn 0/1)
      rm -rf "$W/cut"; mkdir -p "$W/cut"; list=""; cnt=0; first=$at
      while [ $cnt -lt "$CH" ] && [ $at -le "$total" ]; do
        for torn in 0 1; do
          img="$W/cut/c_${at}_${torn}.img"
          cp --sparse=always "$W/base.img" "$img"
          acked=$(USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((at*7+3+${USBMEM_CRASH_SEED_SALT:-0})) "$T" --ops "$img" "$W/ops" 2>&1 | grep -a '^acked:' | sed 's/acked: //' || true)
          [ -n "$acked" ] || acked=0
          echo "$acked" > "$img.acked"
          list="$list $img"
        done
        cnt=$((cnt+1)); at=$((at + STRIDE))
      done
      # ログが未再生か (カーネルがこれから再生するか) を数える
      for img in $list; do xfs_logprint_ok "$img" || dirty_images=$((dirty_images+1)); done
      # (K) 実カーネルで再生 (ツリーも取る)
      python3 -I "$HERE/xfs_vm/kreplay.py" $list >/dev/null 2>&1 || { rc=$?; echo "kreplay failed rc=$rc"; exit 1; }
      # 標本: 実カーネルに続きの書き込みをさせるコピー (チャンクの先頭 2 枚)
      kp=""; for img in $(echo $list | tr ' ' '\n' | head -2); do cp --sparse=always "$img" "$img.kp"; kp="$kp $img.kp"; done
      for img in $list; do
        acked=$(cat "$img.acked"); fa=${img##*/}; tag="sync=$sync_variant mode=$mode ${fa%.img} acked=$acked"
        n=$((n+1))
        cmp -s "$W/base.img" "$img" && same_as_base=$((same_as_base+1)) || changed=$((changed+1))
        kres=$(cat "$img.kres" 2>/dev/null || echo missing)
        case "$kres" in ok) replayed=$((replayed+1)) ;; *) k_fail=$((k_fail+1)); echo "K-FAIL: $tag: kernel mount/replay: $kres"; continue ;; esac
        nfsck=$((nfsck+1))
        xfs_clean "$img" > "$W/fsck.log" 2>&1 || { k_fail=$((k_fail+1)); echo "K-FAIL: $tag: xfs_repair -n not clean after kernel replay"; head -8 "$W/fsck.log"; continue; }
        xfs_logprint_ok "$img" || logprint_bad=$((logprint_bad+1))
        # (S)
        h=$(norm "$img.ktree" | sha256sum | cut -d' ' -f1)
        k_hit=$(grep "^$h " "$W/states.tbl" | head -1 | cut -d' ' -f2 || true)
        if [ -z "$k_hit" ]; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: tree matches no operation boundary state"; norm "$img.ktree" > "$W/n1.tree"; for dk in $acked $((acked + 1)); do norm "$W/st/$(printf '%04d' $dk).tree" > "$W/n0.tree"; echo "  (差: カーネルが見たツリー vs 操作 $dk の後の状態)"; diff "$W/n1.tree" "$W/n0.tree" | head -6; done
        else
          in_states=$((in_states+1))
          # 失敗した操作や同じ状態に戻る操作では隣り合う状態が同一になる (表では最初の番号が引かれる)。acked か acked+1 の状態と同じであれば良い
          if [ $sync_variant = each ] && ! grep -q -e "^$h $acked\$" -e "^$h $((acked + 1))\$" "$W/states.tbl"; then s_fail=$((s_fail+1)); echo "S-FAIL: $tag: state matches op $k_hit (expected $acked or $((acked+1)))"; fi
        fi
        orig_part "$img.ktree" > "$W/c.part"
        cmp -s "$W/orig.part" "$W/c.part" || { a_fail=$((a_fail+1)); echo "A-FAIL: $tag: 元のファイルが変わった/消えた"; diff "$W/orig.part" "$W/c.part" | head -3; }
        # (P) 再生後のイメージに自前ドライバで続きを書く
        cp --sparse=always "$img" "$W/p.img"
        pout=$("$T" --post "$W/p.img" 2>&1) || { p_fail=$((p_fail+1)); echo "P-FAIL: $tag: post driver failed: $pout" | head -3; continue; }
        case "$pout" in
          *"post: done"*)
            post_done=$((post_done+1)); nfsck=$((nfsck+1))
            xfs_clean "$W/p.img" > "$W/fsck.log" 2>&1 || { p_fail=$((p_fail+1)); echo "P-FAIL: $tag: xfs_repair -n not clean after continuing"; head -8 "$W/fsck.log"; } ;;
          *) post_ro=$((post_ro+1)); p_fail=$((p_fail+1)); echo "P-FAIL: $tag: not writable after kernel replay: $pout" ;;
        esac
        rm -f "$img" "$img.ktree" "$img.kres" "$img.acked" "$W/p.img"
      done
      # 実カーネルに続きの書き込みをさせて xfs_repair -n
      if [ -n "$kp" ]; then
        python3 -I "$HERE/xfs_vm/kreplay.py" --post $kp >/dev/null 2>&1 || true
        for img in $kp; do
          kpost_n=$((kpost_n+1)); nfsck=$((nfsck+1))
          if [ "$(cat "$img.kres" 2>/dev/null)" != ok ]; then kp_fail=$((kp_fail+1)); echo "KP-FAIL: sync=$sync_variant mode=$mode ${img##*/}: kernel post-write: $(cat "$img.kres" 2>/dev/null)"; continue; fi
          xfs_clean "$img" > "$W/fsck.log" 2>&1 || { kp_fail=$((kp_fail+1)); echo "KP-FAIL: ${img##*/}: xfs_repair -n not clean after kernel wrote to our volume"; head -8 "$W/fsck.log"; }
        done
      fi
      echo "  ... sync=$sync_variant mode=$mode at=$first..$((at-STRIDE)) 済 (累計 $n)"
    done
  done
done
echo "試行 $n 回 (カットポイント): (K) カーネル再生 / xfs_repair -n が clean でない $k_fail / (S) 操作境界の状態でない・原子性違反 $s_fail / (A) 元ファイル破損 $a_fail / (P) 続きの書き込み後に異常 $p_fail / (KP) カーネルの続きの書き込み後に異常 $kp_fail"
echo "電源断後: 変更なし $same_as_base / 変更あり $changed / 未再生のログが残っていた $dirty_images / カーネルが再生して mount できた $replayed / いずれかの操作境界の状態と一致 $in_states / 続きを書けた $post_done / 書き込み拒否 $post_ro / カーネルの続きの書き込み $kpost_n 回; xfs_repair 計 $nfsck 回"
rc=0
[ "$(sha256sum "$W/base.img" | cut -d" " -f1)" = "$sum0" ] || { echo "FAIL: base image changed"; rc=1; }
[ $k_fail -eq 0 ] && [ $s_fail -eq 0 ] && [ $a_fail -eq 0 ] && [ $p_fail -eq 0 ] && [ $kp_fail -eq 0 ] || rc=1
[ $rc -eq 0 ] && echo "xfs crash: all passed" || echo "xfs crash: FAILED"
exit $rc
