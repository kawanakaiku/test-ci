#!/bin/sh
# FAT12/16/32 書き込みの電源断の模擬 (USBMEM_NTFS_WRITE=ON ビルド用。docs/fat-write.md)。tools/run_exfat_crash_tests.sh と同じ方式:
# 書き込み操作列の N 回目の書き込みで「電源を落とし」(以降の書き込みは消える)、残ったイメージを検証する。
#   prefix = 書き込みは発行順に反映 / cache = flush までバッファして電源断の瞬間に未 flush の書き込みのランダムな部分集合だけが反映 (順序保証のない USB キャッシュ)
#   torn=1 = 電源断の瞬間の 1 回の書き込みをセクタ途中で切る。
# 検査:
#   (B) ブートセクタ (先頭 1 セクタ) 以外のメディアが 1 バイトでも変わっているなら、ボリュームは「汚れている」(ブートセクタのダーティビット) = 最初の変更の前にダーティ印が確定している。
#   (A) fsck.fat -a で直したあと、元からあった全ファイル (操作列が触らないもの) のツリー (種別・サイズ・sha256) が元と一致する。消した元ファイルは「消えている」か「元のまま」。
#   (C) fsck.fat -a のあと fsck.fat -n が clean (終了コード 0)。
#   (D) 修復前の fsck.fat -n の出力に、許容リスト (ダーティ印 / FSInfo の空きクラスタ数・次の割り当て位置 / 未参照クラスタ / FAT の 2 面の差) 以外の指摘が無い
#       = 書き込み順序 (データ → FAT → ディレクトリエントリ) を守っていないと現れる「ディレクトリが空きクラスタを指す」「クロスリンク」「サイズと連鎖長の矛盾」「壊れた長い名前」が出ない。
#   (E) 電源断のあとの 7z による読み出し (修復前でも): 元のファイルが読め、内容が元と一致する (書き込み中に壊れてよいのは操作列が触るファイルだけ)。
#   (F) 修復前に読み出した各ファイルに、空きクラスタの目印 (元のボリュームの空きを 0xEE で埋めてある。tools/fat_fill_free.py) が見えない
#       = 書かれていない新しいクラスタの中身が、ファイルの中身や拡張したディレクトリの中身として見えていない (データ → FAT → エントリの順の検査。新しいディレクトリクラスタのゼロ埋めを FAT より先に書く規則も含む)。
#   (G) 修復前のツリーの名前が、操作列に現れる名前か元からある名前のどれかである (長い名前のスロットが欠けて短い名前 `VERY_L~1.TXT` だけが見える、を検出。tools/fat_crash_check.py)。
#   D/E の注記: FAT12 は電源断の瞬間に FAT の複数セクタの更新が途中までのことがあり、既存ファイルの最後のクラスタから空きクラスタへのリンクだけが残る
#       ("Contains a free cluster. Assuming EOF." — 元のデータには影響しない)。D はそれだけを許容し、7z がイメージ全体を拒否する場合 E は mtools で読む。
# 元のボリュームは**独立実装** (mkfs.fat + mtools) で作る (自前ドライバで作らない)。ボリュームは小さく (または大きなファイルで埋めて) して、操作列の途中で割り当てが一周し、
# 解放済みのクラスタが再利用される状況にする (解放したクラスタの再利用は、解放がコミット (ディスクに届く) されてからでなければならない)。
# 使い方: run_fat_crash_tests.sh <usbmem_fat_write_tests> [未使用: 互換のため] [サンプル数 (既定 40)] [型のリスト (既定 "12 16 32")]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; NS=${3:-40}; TYPES=${4:-"12 16 32"}
for c in mkfs.fat fsck.fat mcopy 7z python3 cmp truncate; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_ftc.XXXXXX)
trap 'rm -rf "$W"' EXIT
export LC_ALL=C.UTF-8 TZ=UTC MTOOLS_SKIP_CHECK=1
export ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0
rc=0
# 許容する fsck.fat -n の指摘 (電源断で通常起こる「ダーティ印」「未参照クラスタ (解放系: エントリを先に消し、FAT を後で解放するため)」「FSInfo の古さ」「FAT の 2 面の差」「FAT32 のブートセクタとバックアップの差 = ダーティビットだけ」)。これ以外は書き込み順序の違反。fsck.fat -a が回収した未参照の連鎖は FSCKnnnn.REC として残るので、元ファイルの比較からは除く
TOLERATED='^ *(fsck\.fat |Dirty bit is set|Automatically removing dirty bit|Reclaimed [0-9]+ unused|Free cluster summary|Auto-correcting|Leaving filesystem unchanged|Not automatically fix|FATs differ|Using first FAT|There are differences between boot sector and its backup|This is mostly harmless|Differences: |Contains a free cluster \([0-9]+\)\. Assuming EOF)|^/[^ ]*$|^ *65:|: [0-9]+ files, |^Orphaned long file name part|^ *Auto-deleting\.|^ *$'
# 元のボリューム: type → (mkfs の引数, 大きさ, 埋めるファイルの大きさ)
setup() {  # setup <type> <img>
  case $1 in
    12) F=12; SZ=1440K; FILL=0 ;;
    16) F=16; SZ=3M; FILL=0 ;;
    32) F=32; SZ=36M; FILL=33500000 ;;
  esac
  rm -f "$2"; truncate -s $SZ "$2"; mkfs.fat -F $F -s 1 -R 32 "$2" >/dev/null 2>&1 || return 1
  rm -rf "$W/pop"; mkdir "$W/pop"
  python3 -I - "$W/pop" "$FILL" "$1" <<'PY'
import sys, os
root, fill, typ = sys.argv[1], int(sys.argv[2]), sys.argv[3]
def w(p, n, seed):
    os.makedirs(os.path.dirname(os.path.join(root, p)), exist_ok=True)
    open(os.path.join(root, p), "wb").write(bytes((((i * 7) + seed) & 255) for i in range(n)))
os.makedirs(os.path.join(root, "od"))
for i, n in enumerate([3000, 9000, 20000, 500, 6000, 12000, 4096, 4097], 1): w("orig%d.bin" % i, n, 40 + i)
for i in range(1, 6): w("od/o%d.bin" % i, 7000 + 1000 * i, 60 + i)
w("日本語の長い名前のファイル.txt", 777, 99)
if fill: w("filler.bin", fill, 5)      # 空きを少しだけ残す (割り当てが一周して、解放済みのクラスタが再利用される)
PY
  mcopy -s -m -i "$2" "$W"/pop/* :: >/dev/null 2>&1 || return 1
  # 空きクラスタを目印 (0xEE の連続) で埋める: 書かれていない新しいクラスタの中身が、電源断のあと見えてしまうこと (データより先にエントリ/FAT が届いた) を検出するため (F)。
  # 目印はデータ領域のクラスタ内だけ。FAT・ディレクトリエントリ・元のファイルは変えない (fsck.fat -n は clean のまま)。
  python3 -I "$HERE/fat_fill_free.py" "$2" || return 1
}
tree_of() {  # tree_of <img> <out>
  rm -rf "$W/xt"; mkdir "$W/xt"; 7z x -y -o"$W/xt" "$1" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/xt" > "$2"
}
# 操作列が触る場所と、消す元ファイル (orig3, o2, orig5) を除いた「元からあるもの」のダンプ
treemc() {  # treemc <img> <out>: mtools で展開 (7z が拒否するイメージ用)
  rm -rf "$W/xm"; mkdir "$W/xm"; mcopy -s -n -i "$1" :: "$W/xm" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/xm" > "$2"
}
orig_part() { grep -v -E ' (cr|top\.txt|top_[0-9]+\.bin|late\.bin|orig3\.bin|orig5\.bin|od/o2\.bin|FSCK[0-9]+\.REC)(/|$)' "$1" || true; }
deleted_part() { grep -E ' (orig3\.bin|orig5\.bin|od/o2\.bin)$' "$1" || true; }
# ブートセクタのダーティビット (FAT12/16: 0x25、FAT32: 0x41 の bit0)
boot_dirty() { python3 -I -c "
import sys
d=open(sys.argv[1],'rb').read(512)
off=0x41 if d[22]==0 and d[23]==0 else 0x25
print(d[off]&1)" "$1"; }
TOTALN=0; TOTAL_A=0; TOTAL_B=0; TOTAL_C=0; TOTAL_D=0; TOTAL_E=0; TOTAL_F=0; TOTAL_G=0
for typ in $TYPES; do
  setup $typ "$W/orig.img" || { echo "skip: cannot build the original FAT$typ volume" >&2; continue; }
  fsck.fat -n "$W/orig.img" >/dev/null 2>&1 || { echo "skip: original FAT$typ volume not clean" >&2; continue; }
  tree_of "$W/orig.img" "$W/orig.tree" || { echo "7z failed on the original" >&2; continue; }
  orig_part "$W/orig.tree" > "$W/orig.part"; deleted_part "$W/orig.tree" > "$W/orig.del"
  # 操作列: 空きの大部分を使うファイル → 解放 → 別の大きなファイル (割り当てが一周して解放済みクラスタを再利用する)
  python3 -I - "$W/ops_main" "$typ" <<'PY'
import sys
typ = sys.argv[2]
big = {"12": 700000, "16": 1500000, "32": 1800000}[typ]
L = ["unlink /orig3.bin", "unlink /od/o2.bin", "mkdir /cr", "mkdir /cr/sub"]
for i in range(6): L += ["create /top_%d.bin" % i, "write /top_%d.bin 0 %d %d" % (i, 14000, 90 + i)]
for i in range(1, 25): L += ["create /cr/f%d.bin" % i, "write /cr/f%d.bin 0 %d %d" % (i, [100, 12000, 30000, 300][i % 4], i)]
for i in range(14): L += ["create /cr/very_long_file_name_number_%02d_for_the_lfn_slot_test.txt" % i, "write /cr/very_long_file_name_number_%02d_for_the_lfn_slot_test.txt 0 %d %d" % (i, 200 + 50 * i, 120 + i)]   # 長い名前 (LFN のスロット列がセクタ境界をまたぐ) を多数
L += ["create /cr/sub/x.txt", "write /cr/sub/x.txt 0 9000 7", "rename /cr/f1.bin /cr/sub/moved.bin"]
for i in range(2, 24, 2): L.append("unlink /cr/f%d.bin" % i)
L += ["create /cr/big.bin", "write /cr/big.bin 0 %d 3" % big, "write /cr/f3.bin 0 20000 77", "rename /cr/sub /cr/sub2", "truncate /cr/f7.bin 10"]
L += ["unlink /cr/big.bin", "create /cr/big2.bin", "write /cr/big2.bin 0 %d 4" % (big * 2 // 3)]       # 解放 → 直後に別の大きな確保 (一周して再利用)
L += ["create /top.txt", "write /top.txt 0 700 5", "rename /cr/f9.bin /cr/f3.bin", "unlink /orig5.bin", "create /late.bin", "write /late.bin 0 30000 9",
      "create /cr/日本語の名前の長いファイル.txt", "write /cr/日本語の名前の長いファイル.txt 0 5000 6", "mkdir /cr/Long_Directory_Name_Here", "rename /cr/日本語の名前の長いファイル.txt /cr/Long_Directory_Name_Here/moved_日本語.txt"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
  # 全書き込み位置を 1 つずつ止める短い操作列 (dense): ディレクトリの拡張 (新しいクラスタの中身 → FAT のつなぎ → エントリ)・長い名前のスロット列がセクタ境界をまたぐ追加・改名。
  # main はサンプリングなので、書き込み順序の 1 ステップの違い (FAT のつなぎだけ先に届く、短い名前だけ先に届く等) を取りこぼしやすい。
  python3 -I - "$W/ops_dense" <<'PY'
import sys
L = ["mkdir /cr"]
for i in range(20): L.append("create /cr/s%d.txt" % i)
for i in range(10):
    nm = "/cr/dense_long_file_name_number_%02d_for_the_lfn_slot_test.txt" % i
    L += ["create " + nm, "write %s 0 %d %d" % (nm, 100 + 700 * (i % 3), 30 + i)]
for i in range(0, 10, 3): L.append("rename /cr/dense_long_file_name_number_%02d_for_the_lfn_slot_test.txt /cr/renamed_long_file_name_number_%02d_for_the_lfn_slot_test.txt" % (i, i))
for i in range(0, 20, 4): L.append("unlink /cr/s%d.txt" % i)
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
  rc_t=0; n=0; a_fail=0; b_fail=0; c_fail=0; d_fail=0; e_fail=0; f_fail=0; g_fail=0; dirty_after=0; clean_after=0; inconsistent=0; fsck_fixed=0
  for scen in main dense; do
  cp "$W/ops_$scen" "$W/ops"
  if [ $scen = dense ]; then MODES=prefix; TORNS=0; else MODES="prefix cache"; TORNS="0 1"; fi
  for sync_variant in batch each; do
    if [ $sync_variant = each ]; then export USBMEM_SYNC_EVERY=1; else unset USBMEM_SYNC_EVERY || true; fi
    cp "$W/orig.img" "$W/c.img"
    USBMEM_CRASH_AT=-1 "$T" --ops "$W/c.img" "$W/ops" > "$W/base.log" 2>&1 || true
    total=$(grep -a "^writes:" "$W/base.log" | sed 's/writes: //')
    echo "FAT$typ: 操作列の総書き込み数: $total (sync=$sync_variant); $(grep -a '^ops:' "$W/base.log")"
    grep -a '^op failed' "$W/base.log" | head -3 | sed 's/^/  /' || true
    [ -n "$total" ] && [ "$total" -gt 20 ] || { echo "FAIL: 総書き込み数が取れない"; exit 1; }
    if [ $scen = dense ]; then NSX=$total; else NSX=$NS; fi   # dense: 全書き込み位置 (at=total は電源断なしの完走 = クリーンに閉じた状態なので含めない)
    for mode in $MODES; do
      i=0
      while [ $i -lt $NSX ]; do
        # 序盤を密に、後半を粗くサンプリング
        if [ $scen = dense ]; then at=$i; elif [ $i -lt 15 ]; then at=$i; else at=$(( total * (i - 14) / (NS - 14) )); fi
        for torn in $TORNS; do
          cp "$W/orig.img" "$W/c.img"
          USBMEM_CRASH_AT=$at USBMEM_CRASH_MODE=$mode USBMEM_CRASH_TORN=$torn USBMEM_CRASH_SEED=$((i*7+3)) "$T" --ops "$W/c.img" "$W/ops" >/dev/null 2>&1 || true
          n=$((n+1)); cp "$W/c.img" "$W/c0.img"
          # (B) ブートセクタ (先頭 512 バイト) 以外が変わっているなら、ダーティビットが立っている
          chg=$(cmp -l "$W/orig.img" "$W/c.img" | awk '$1 > 512' | wc -l)
          bd=$(boot_dirty "$W/c.img")
          if [ "$chg" -gt 0 ] && [ "$bd" -eq 0 ]; then b_fail=$((b_fail+1)); echo "B-FAIL: FAT$typ sync=$sync_variant mode=$mode at=$at torn=$torn: ${chg} bytes changed outside the boot sector but the dirty bit is not set"; fi
          if [ "$bd" -ne 0 ]; then dirty_after=$((dirty_after+1)); else clean_after=$((clean_after+1)); fi
          # (D) 修復前: 許容リスト以外の指摘が無い (torn=1 は 1 回の複数セクタ書き込みの途中切れがあり得る: ディレクトリクラスタの末尾が古い等。FAT は複数セクタの原子性を持たない)
          fsck.fat -n "$W/c.img" > "$W/pre.log" 2>&1 && r=0 || r=$?
          if [ $r -ne 0 ]; then inconsistent=$((inconsistent+1)); fi
          bad=$(python3 -I "$HERE/fat_crash_check.py" fsckfilter "$W/pre.log" | grep -a -v -E "$TOLERATED" | head -3 | tr '\n' '|')
          if [ -n "$bad" ]; then d_fail=$((d_fail+1)); echo "D-FAIL: FAT$typ 修復前に不整合 (sync=$sync_variant mode=$mode at=$at torn=$torn): $bad"; fi
          [ -z "${FC_VERBOSE:-}" ] || { echo "  pre-repair (sync=$sync_variant mode=$mode at=$at torn=$torn) rc=$r:"; grep -a -v "^fsck.fat" "$W/pre.log" | head -6 | sed "s/^/    /"; }
          # (E) 修復前の読み出し: 元からあるファイルが元のまま読める。7z は FAT の連鎖が長さに合わないファイルがあるとイメージ全体を拒否することがある
          #     (下の「FAT 拡張の途中切れ」= 電源断の瞬間に FAT の複数セクタの更新が途中までのとき) ので、その場合は mtools (連鎖を寛容に辿る) で読む
          xd=""
          if tree_of "$W/c.img" "$W/e.tree"; then xd=$W/xt; elif grep -a -q "Contains a free cluster" "$W/pre.log" && treemc "$W/c.img" "$W/e.tree"; then xd=$W/xm; fi
          if [ -n "$xd" ]; then
            orig_part "$W/e.tree" > "$W/e.part"
            if ! cmp -s "$W/orig.part" "$W/e.part"; then e_fail=$((e_fail+1)); echo "E-FAIL: FAT$typ 修復前に元のファイルが変わった/読めない (sync=$sync_variant mode=$mode at=$at torn=$torn)"; diff "$W/orig.part" "$W/e.part" | head -3; fi
            # (F) 空きクラスタの目印がファイルの中身として見えていない / (G) 壊れた名前が見えていない
            fo=$(python3 -I "$HERE/fat_crash_check.py" marker "$xd") || { f_fail=$((f_fail+1)); echo "F-FAIL: FAT$typ 書かれていないクラスタの中身がファイルとして見える: $fo (sync=$sync_variant mode=$mode at=$at torn=$torn)"; }
            go=$(python3 -I "$HERE/fat_crash_check.py" names "$W/ops" "$W/orig.tree" "$W/e.tree") || { g_fail=$((g_fail+1)); echo "G-FAIL: FAT$typ 壊れた名前が見える: $go (sync=$sync_variant mode=$mode at=$at torn=$torn)"; }
          else e_fail=$((e_fail+1)); echo "E-FAIL: FAT$typ 7z が修復前のイメージを読めない (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
          # (C)(A) fsck.fat -a で直す → clean → 元のツリーが同じ
          fsck.fat -a "$W/c.img" >"$W/fsck.log" 2>&1 && r=0 || r=$?
          [ $r -ne 0 ] && fsck_fixed=$((fsck_fixed+1))
          if [ $r -ge 2 ]; then c_fail=$((c_fail+1)); echo "C-FAIL: FAT$typ fsck.fat -a exit $r (sync=$sync_variant mode=$mode at=$at torn=$torn)"; tail -3 "$W/fsck.log"; continue; fi
          if ! fsck.fat -n "$W/c.img" >"$W/post.log" 2>&1; then c_fail=$((c_fail+1)); echo "C-FAIL: FAT$typ not clean after fsck.fat -a (sync=$sync_variant mode=$mode at=$at torn=$torn)"; head -5 "$W/post.log"; continue; fi
          if tree_of "$W/c.img" "$W/c.tree"; then
            orig_part "$W/c.tree" > "$W/c.part"
            deleted_part "$W/c.tree" > "$W/c.del"
            if [ -s "$W/c.del" ] && [ -n "$(grep -vxFf "$W/orig.del" "$W/c.del" || true)" ]; then a_fail=$((a_fail+1)); echo "A-FAIL: FAT$typ 消した元ファイルが別の内容で残っている (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
            if ! cmp -s "$W/orig.part" "$W/c.part"; then a_fail=$((a_fail+1)); echo "A-FAIL: FAT$typ 元のファイルが変わった/消えた (sync=$sync_variant mode=$mode at=$at torn=$torn)"; diff "$W/orig.part" "$W/c.part" | head -3; fi
          else a_fail=$((a_fail+1)); echo "A-FAIL: FAT$typ fsck 後に 7z で読めない (sync=$sync_variant mode=$mode at=$at torn=$torn)"; fi
        done
        i=$((i+1))
      done
    done
  done
  done
  echo "FAT$typ: 試行 $n 回: (A) 元ファイル破損 $a_fail / (B) ダーティ印なしで変更 $b_fail / (C) fsck 後に clean にならない $c_fail / (D) 修復前に許容外の指摘 $d_fail / (E) 修復前に元ファイルが読めない $e_fail / (F) 書かれていないクラスタの露出 $f_fail / (G) 壊れた名前 $g_fail"
  echo "FAT$typ: 電源断後の状態: 汚れている(要 fsck) $dirty_after / clean のまま $clean_after; fsck.fat -n が非 0 だった試行 $inconsistent; fsck.fat -a が修復した試行 $fsck_fixed"
  TOTALN=$((TOTALN+n)); TOTAL_A=$((TOTAL_A+a_fail)); TOTAL_B=$((TOTAL_B+b_fail)); TOTAL_C=$((TOTAL_C+c_fail)); TOTAL_D=$((TOTAL_D+d_fail)); TOTAL_E=$((TOTAL_E+e_fail)); TOTAL_F=$((TOTAL_F+f_fail)); TOTAL_G=$((TOTAL_G+g_fail))
done
echo "合計 $TOTALN 回: (A) $TOTAL_A / (B) $TOTAL_B / (C) $TOTAL_C / (D) $TOTAL_D / (E) $TOTAL_E / (F) $TOTAL_F / (G) $TOTAL_G"
[ "$TOTALN" -gt 0 ] || { echo "skip: no volume could be built" >&2; exit 77; }
[ "$TOTAL_A" -eq 0 ] && [ "$TOTAL_B" -eq 0 ] && [ "$TOTAL_C" -eq 0 ] && [ "$TOTAL_D" -eq 0 ] && [ "$TOTAL_E" -eq 0 ] && [ "$TOTAL_F" -eq 0 ] && [ "$TOTAL_G" -eq 0 ] || rc=1
exit $rc
