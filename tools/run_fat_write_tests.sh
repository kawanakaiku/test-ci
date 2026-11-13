#!/bin/sh
# FAT12/16/32 (vfat) 書き込みの検証 (USBMEM_NTFS_WRITE=ON ビルド用。docs/fat-write.md)。自前ドライバ (fs/fat を LinuxCompat 上で動かしたもの) で書いたイメージを、
# 独立実装 (dosfstools の fsck.fat、mtools、7-Zip) で検証する。ルート権限もカーネルのマウントも使わない (カーネルの vfat との突き合わせは tools/run_fat_kernel_tests.sh: CI のみ)。
#   1. 決まった操作列 (フィクスチャ 8 種 = FAT12/16/32 x クラスタ 512B..8KiB x セクタ 512/4096): 自前ドライバで実行 → fsck.fat -n が clean (終了コード 0 = FAT1/FAT2 の一致・
#      FSInfo の空きクラスタ数・連鎖・重複名・LFN の検査を含む) → 同じ操作列を**ホストのディレクトリ** (7z で展開した元イメージ) に POSIX 操作として実行した結果と、
#      自前ドライバが書いたイメージを 7z で展開したツリー (種別・サイズ・sha256)、および mtools (mcopy) で展開したツリー (mtools が扱えない名前を除く) が一致すること。
#   2. 乱数操作 (メモリ上のモデルとの突き合わせ + 再マウント後の比較) を mkfs.fat で作った 10 種のボリューム (FAT12/16/32 x クラスタ 512B..64KiB x セクタ 512/4096) で → fsck.fat clean、
#      モデルのダンプが 7z で展開したツリーと一致。
#   3. 数千ファイルの作成・削除・改名 (大きなディレクトリ: クラスタをまたいで伸びる、消した跡の再利用) → fsck.fat clean、ホストの POSIX 操作の結果と一致。
#   4. 何も書かないこと / ダーティ・MediaFailure・FAT[1] 未クリーンのイメージは読み取り専用でイメージ不変 / 満杯・ディレクトリ伸長不能・FAT12/16 の固定ルート枯渇で安全に失敗し回復 /
#      名前の規則 (8.3 名の衝突・UTF-16・大文字小文字) / 4GiB-1 の上限 (EFBIG) / ダーティ印とクリーン unmount (ブートセクタ・FAT[1]・FSInfo)。
#   6. パーティションのオフセット (イメージの途中に FAT がある): 前後の領域は不変、ボリュームは単体のときと同じ結果。
#   5. ファジング: メタデータを壊したイメージに rw マウントして乱数操作 (クラッシュ・ハングしない。ASan 下で有効)。
# 使い方: run_fat_write_tests.sh <usbmem_fat_write_tests> [フィクスチャ dir (tools/gen_fat_fixture.py)] [モデル試行数 (既定 2)]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
T=$1; D=${2:-/dev/shm/usbmem_fat_${USER:-u}}; NM=${3:-2}
for c in mkfs.fat fsck.fat mcopy 7z python3 truncate; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/fat32_c4k.img" ] && [ -f "$D/fat12_f1440.img" ] || { echo "skip: no fixture in $D (tools/gen_fat_fixture.py)" >&2; exit 77; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d /dev/shm/usbmem_ftw.XXXXXX)
trap 'rm -rf "$W"' EXIT
export LC_ALL=C.UTF-8 TZ=UTC MTOOLS_SKIP_CHECK=1
export ASAN_OPTIONS=${ASAN_OPTIONS:-verify_asan_link_order=0:detect_leaks=1}
rc=0
fail() { echo "FAIL: $*"; rc=1; }
fsck_clean() {  # fsck_clean <img> <label>
  out=$(fsck.fat -n "$1" 2>&1) || { echo "$out" | head -20; fail "$2: fsck.fat not clean"; return 1; }
}
tree7z() {  # tree7z <img> <out>: 7z で展開してツリーを出力
  rm -rf "$W/x7"; mkdir "$W/x7"; 7z x -y -o"$W/x7" "$1" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/x7" > "$2"
}
treemc() {  # treemc <img> <out>: mtools で展開 (BMP 外の文字を含む名前は '_' に化けるので、その行は両側から除いて比べる)
  rm -rf "$W/xm"; mkdir "$W/xm"; mcopy -s -n -i "$1" :: "$W/xm" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/xm" | grep -v -E '___|😀' > "$2" || true
}
nonbmp() { grep -v -E '___|😀' "$1" || true; }

# ---------- 1. 決まった操作列 ----------
for fx in fat12_f1440:512:200000:100 fat12_c4k:4096:1500000:150 fat16_c512:512:3000000:200 fat16_c2k:2048:3000000:300 fat32_c512:512:3000000:300 fat32_c4k:4096:3000000:300 fat32_s4k:4096:3000000:300 fat32_c8k:8192:3000000:300; do
  IFS=: read name cl big nmany <<EOF
$fx
EOF
  IMG=$D/$name.img
  python3 -I "$HERE/gen_fat_ops.py" $cl "$W/ops_$name.txt" $nmany $big
  cp "$IMG" "$W/a_$name.img"
  tree7z "$IMG" "$W/t0" || { fail "7z extract of the original ($name)"; continue; }
  rm -rf "$W/ref"; cp -r "$W/x7" "$W/ref"
  out=$("$T" --ops "$W/a_$name.img" "$W/ops_$name.txt" 2>&1) || { echo "$out" | tail -5; fail "ops $name: driver"; continue; }
  echo "[$name] $(echo "$out" | grep -a '^ops:')"
  fsck_clean "$W/a_$name.img" "ops $name" || continue
  python3 -I "$HERE/ops_ref.py" "$W/ref" "$W/ops_$name.txt" || { fail "reference ops ($name)"; continue; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/ref" > "$W/tr"
  tree7z "$W/a_$name.img" "$W/ta" || { fail "7z extract ($name)"; continue; }
  if cmp -s "$W/ta" "$W/tr"; then n7=$(wc -l < "$W/ta"); else diff "$W/ta" "$W/tr" | head -10; fail "ops $name: 7z tree differs from the host-reference tree"; continue; fi
  treemc "$W/a_$name.img" "$W/tm" || { fail "mcopy extract ($name)"; continue; }
  nonbmp "$W/tr" > "$W/tr2"
  if cmp -s "$W/tm" "$W/tr2"; then echo "[$name] tree identical to the host reference ($n7 entries via 7z; mtools agrees on $(wc -l < "$W/tm")), fsck.fat clean"; else diff "$W/tm" "$W/tr2" | head -10; fail "ops $name: mtools tree differs"; fi
done

# ---------- 2. 乱数操作 + モデル ----------
# name:F:spc:S:size:small
CONFIGS="f12c512:12:1:512:1440K:small f12c4k:12:8:512:12M:small f16c512:16:1:512:20M:small f16c2k:16:4:512:64M: f16c32k:16:64:512:200M: f16s4k:16:1:4096:100M: f32c512:32:1:512:40M: f32c4k:32:8:512:300M: f32s4k:32:1:4096:280M: f32c64k:32:128:512:4200M:"
mkimg() {  # mkimg <path> <F> <spc> <S> <size>
  rm -f "$1"; truncate -s "$5" "$1"; mkfs.fat -F "$2" -s "$3" -S "$4" -R 32 "$1" >/dev/null 2>&1 || { echo "FAIL: mkfs.fat $*" >&2; exit 1; }
}
i=1
for cfg in $CONFIGS; do
  IFS=: read name F spc S size small <<EOF
$cfg
EOF
  for k in $(seq 1 "$NM"); do
    seed=$((i * 1000 + k)); img=$W/m.img
    mkimg "$img" $F $spc $S $size
    out=$("$T" --model "$img" $seed 1200 "$W/m.dump" $small 2>&1) || { echo "$out" | tail -8; fail "model $name seed=$seed"; continue; }
    fsck_clean "$img" "model $name seed=$seed" || continue
    tree7z "$img" "$W/m.tree" || { fail "7z extract (model $name)"; continue; }
    cmp -s "$W/m.dump" "$W/m.tree" || { diff "$W/m.dump" "$W/m.tree" | head -5; fail "model $name seed=$seed: 7z tree differs from the model"; continue; }
    echo "[model $name seed=$seed] $(echo "$out" | grep -a 'model:' | cut -c1-110); fsck.fat clean; 7z tree matches"
  done
  i=$((i + 1))
done

# ---------- 3. 数千ファイルの作成・削除 (大きなディレクトリ: クラスタをまたいで伸びる、消した跡の再利用) ----------
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
for cfg in f16c512:16:1:512:30M f32c512:32:1:512:64M f32c4k:32:8:512:300M f32s4k:32:1:4096:280M; do
  IFS=: read name F spc S size <<EOF
$cfg
EOF
  img=$W/big.img; mkimg "$img" $F $spc $S $size
  rm -rf "$W/ref"; mkdir "$W/ref"
  out=$("$T" --ops "$img" "$W/many.ops" 2>&1) || { echo "$out" | tail -5; fail "many files $name: driver"; continue; }
  fsck_clean "$img" "many files $name" || continue
  python3 -I "$HERE/ops_ref.py" "$W/ref" "$W/many.ops"; python3 -I "$HERE/ops_ref.py" --tree "$W/ref" > "$W/tr"
  tree7z "$img" "$W/ta" && cmp -s "$W/ta" "$W/tr" && echo "[many files $name] $(echo "$out" | grep -a 'ops:' | tail -1); $(wc -l < "$W/ta") entries identical to the host reference; fsck.fat clean" || fail "many files $name: tree differs"
done

# ---------- 4. 何も書かない / 書き込み拒否 / 満杯 / 名前 / 上限 / クリーン unmount ----------
run_t() {  # run_t <label> <args...>: 失敗したら FAIL、最後の出力行を表示
  label=$1; shift
  if out=$("$T" "$@" 2>&1); then echo "[$label] $(echo "$out" | tail -1)"; else echo "$out" | tail -5; fail "$label"; fi
}
for fx in fat12_f1440 fat16_c512 fat32_c4k fat32_s4k; do cp "$D/$fx.img" "$W/n.img"; run_t "noop $fx (rw mount, no writes: image unchanged)" --noop "$W/n.img"; done
for variant in boot-dirty boot-media fat-unclean fat-unclean-2; do
  for fx in fat12_f1440 fat16_c2k fat32_c4k fat32_s4k; do cp "$D/$fx.img" "$W/r.img"; run_t "refuse $variant $fx" --refuse "$W/r.img" $variant; done
done
SMALLS="f12:12:1:512:1440K f16:16:1:512:8M f32:32:1:512:36M f16s4k:16:1:4096:20M f32s4k:32:1:4096:280M"
for cfg in $SMALLS; do
  IFS=: read name F spc S size <<EOF
$cfg
EOF
  mkimg "$W/s.img" $F $spc $S $size; run_t "enospc $name" --enospc "$W/s.img"; fsck_clean "$W/s.img" "enospc $name" || true
  mkimg "$W/s.img" $F $spc $S $size; run_t "names $name" --names "$W/s.img"; fsck_clean "$W/s.img" "names $name" || true
  mkimg "$W/s.img" $F $spc $S $size; run_t "limits $name" --limits "$W/s.img"; fsck_clean "$W/s.img" "limits $name" || true
  mkimg "$W/s.img" $F $spc $S $size; run_t "clean unmount marks $name" --clean "$W/s.img"; fsck_clean "$W/s.img" "clean $name" || true
done
# フィクスチャ (mtools で作った、実物に近い内容) でもダーティ印・クリーン unmount
for fx in fat12_f1440 fat16_c512 fat32_c512 fat32_s4k; do cp "$D/$fx.img" "$W/s.img"; run_t "clean unmount marks $fx" --clean "$W/s.img"; fsck_clean "$W/s.img" "clean $fx" || true; done

# ---------- 6. パーティションのオフセット ----------
# FAT ボリュームが大きなイメージの途中 (MBR/GPT のパーティション) にある場合: 前 (オフセット分) と後ろの領域は 1 バイトも変わらず、ボリューム部分は単体のときと同じ結果になること
# (USBMEM_IMG_OFFSET / USBMEM_IMG_TAIL: テストバイナリが SubRangeDevice で開く)。オフセットは 63 セクタ (旧 MBR の先頭パーティション) と 1MiB+4096。
for fx in fat12_c4k:4096:1500000:150:32256 fat16_c512:512:3000000:200:1052672 fat32_s4k:4096:3000000:300:1052672 fat32_c512:512:3000000:300:32256; do
  IFS=: read name cl big nmany off <<EOF
$fx
EOF
  python3 -I "$HERE/gen_fat_ops.py" $cl "$W/ops_o.txt" $nmany $big
  tail_n=8192
  head -c "$off" /dev/zero | tr '\0' '\245' > "$W/pre.bin"; head -c "$tail_n" /dev/zero | tr '\0' '\132' > "$W/post.bin"
  cat "$W/pre.bin" "$D/$name.img" "$W/post.bin" > "$W/part.img"
  out=$(USBMEM_IMG_OFFSET=$off USBMEM_IMG_TAIL=$tail_n "$T" --ops "$W/part.img" "$W/ops_o.txt" 2>&1) || { echo "$out" | tail -5; fail "offset $name: driver"; continue; }
  sz=$(stat -c %s "$D/$name.img")
  head -c "$off" "$W/part.img" > "$W/pre2.bin"; tail -c "$tail_n" "$W/part.img" > "$W/post2.bin"
  cmp -s "$W/pre.bin" "$W/pre2.bin" || { fail "offset $name: the area before the partition changed"; continue; }
  cmp -s "$W/post.bin" "$W/post2.bin" || { fail "offset $name: the area after the partition changed"; continue; }
  tail -c +$((off + 1)) "$W/part.img" | head -c "$sz" > "$W/inner.img"
  fsck_clean "$W/inner.img" "offset $name" || continue
  cp "$D/$name.img" "$W/solo.img"; "$T" --ops "$W/solo.img" "$W/ops_o.txt" > /dev/null 2>&1 || { fail "offset $name: solo run"; continue; }
  if tree7z "$W/inner.img" "$W/to1" && tree7z "$W/solo.img" "$W/to2" && cmp -s "$W/to1" "$W/to2"; then echo "[offset $name @$off] ok: outside areas untouched, fsck.fat clean, tree identical to the un-offset run ($(wc -l < "$W/to1") entries)"; else fail "offset $name: tree differs from the un-offset run"; fi
done

# ---------- 5. ファジング ----------
for fx in fat12_f1440 fat16_c512 fat32_c512; do ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 run_t "fuzz $fx" --fuzz "$D/$fx.img" 7 ${FAT_FUZZ_ITERS:-40}; done

[ $rc -eq 0 ] && echo "fat write: all passed" || echo "fat write: FAILED"
exit $rc
