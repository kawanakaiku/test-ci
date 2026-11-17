#!/bin/bash
# fuseshim の試験 (docs/fuse-layer.md 5 章): 同じ NTFS イメージを (a) 自前の NTFS (usbmem_fsdump_core) と (b) shim 越しの ntfs-3g
# (usbmem_fsdump_ntfs3g) で読み、ツリー (名前・種別・サイズ・sha256) が一致することを確かめ、性能を測る。
# 使い方: tools/run_fuseshim_tests.sh <ビルドディレクトリ (実行体がある)> <作業ディレクトリ>
# 前提: mkntfs / ntfs-3g / fusermount3 / python3 (フィクスチャ生成)。無ければ失敗 (skip しない。専用ジョブでだけ動かす)。
# 環境変数: FUSESHIM_STRICT_MTIME=1 で mtime の不一致も失敗にする (既定は報告のみ)。
set -u
B=${1:?build dir}; W=${2:?work dir}
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$B/usbmem_fsdump_core; N3G=$B/usbmem_fsdump_ntfs3g
[ -x "$CORE" ] && [ -x "$N3G" ] || { echo "実行体が無い: $CORE $N3G" >&2; exit 1; }
mkdir -p "$W"
fail=0
SUM=${GITHUB_STEP_SUMMARY:-/dev/null}
say() { echo "$@"; echo "$@" >> "$SUM"; }
export LC_ALL=C

strip_mtime() { sed -E 's/^(. [^ ]+) [^ ]+ /\1 - /' "$1" | sort; }

compare_one() {  # name image manifest
  local name=$1 img=$2 man=$3 d=$W/$1
  mkdir -p "$d"
  "$CORE" "$img" > "$d/core.txt" 2> "$d/core.err"; local rc1=$?
  "$N3G" "$img" > "$d/n3g.txt" 2> "$d/n3g.err"; local rc2=$?
  say "### $name"
  say "- core: rc=$rc1 $(tail -1 "$d/core.err")"
  say "- ntfs-3g (shim): rc=$rc2 $(tail -1 "$d/n3g.err")"
  if [ $rc1 -ne 0 ] || [ $rc2 -ne 0 ]; then
    echo "--- core.err"; cat "$d/core.err"; echo "--- n3g.err"; cat "$d/n3g.err"
    say "- FAIL: 実行体が失敗"; fail=1; return
  fi
  local n1 n2; n1=$(wc -l < "$d/core.txt"); n2=$(wc -l < "$d/n3g.txt")
  strip_mtime "$d/core.txt" > "$d/core.nomtime"; strip_mtime "$d/n3g.txt" > "$d/n3g.nomtime"
  if diff -u "$d/core.nomtime" "$d/n3g.nomtime" > "$d/diff.txt"; then
    say "- ツリー比較 (名前・種別・サイズ・sha256): 一致 ($n1 行 / $n2 行)"
  else
    say "- FAIL: ツリー比較で不一致 ($n1 行 / $n2 行)。先頭:"; head -40 "$d/diff.txt" | tee -a "$SUM"; fail=1
  fi
  if [ "$n1" -lt 100 ]; then say "- FAIL: 行数が少なすぎる ($n1)"; fail=1; fi
  # mtime (秒)
  sort "$d/core.txt" > "$d/core.sorted"; sort "$d/n3g.txt" > "$d/n3g.sorted"
  local md; md=$(diff "$d/core.sorted" "$d/n3g.sorted" | grep -c '^<' || true)
  say "- mtime を含めた比較: 不一致 $md 行"
  if [ "$md" -ne 0 ]; then diff "$d/core.sorted" "$d/n3g.sorted" | head -10 | tee -a "$SUM"; [ "${FUSESHIM_STRICT_MTIME:-0}" = 1 ] && fail=1; fi
  # 生成時のマニフェスト (mkntfs + ntfs-3g (FUSE) で書いた時の sha256) との照合: マニフェストの全エントリが n3g.txt に有ること
  if [ -n "$man" ] && [ -f "$man" ]; then
    sed -E 's#^f ([0-9]+) ([0-9a-f]+) (.*)$#f \1 - \2 /\3#; s#^d [0-9]+ - (.*)$#d - - - /\1#' "$man" | sort > "$d/manifest.conv"
    local miss; miss=$(comm -23 "$d/manifest.conv" "$d/n3g.nomtime" | wc -l)
    if [ "$miss" -eq 0 ]; then say "- マニフェスト照合 (shim 越しの ntfs-3g): $(wc -l < "$d/manifest.conv") エントリ全部一致"
    else say "- FAIL: マニフェストに有って shim 越しの出力に無い/違う: $miss 行"; comm -23 "$d/manifest.conv" "$d/n3g.nomtime" | head -10 | tee -a "$SUM"; fail=1; fi
  fi
}

# ---- 1. 基本フィクスチャ (tools/gen_ntfs_fixture.sh: 圧縮 LZNT1・スパース・800 ファイルのディレクトリ・日本語名)
for cl in 4096 2048; do
  fx=$W/basic_c$cl
  rm -rf "$fx"
  USBMEM_NTFS_CLUSTER=$cl sh "$HERE/gen_ntfs_fixture.sh" "$fx" > "$fx.gen.log" 2>&1 || { cat "$fx.gen.log"; echo "フィクスチャ生成に失敗 (cluster $cl)" >&2; exit 1; }
  compare_one "basic cluster=$cl" "$fx/ntfs_basic.img" "$fx/ntfs_basic.manifest"
done

# ---- 2. 性能 (イメージは 1 つ。自前 → ntfs-3g を 2 回交互に)
sh "$HERE/gen_fuseshim_perf_fixture.sh" "$W/perf" > "$W/perf.gen.log" 2>&1 || { cat "$W/perf.gen.log"; echo "性能用フィクスチャ生成に失敗" >&2; exit 1; }
compare_one "perf image (全ツリー比較)" "$W/perf/ntfs_perf.img" ""
say "### 性能 (CI のマシンの値は参考。同じイメージ・同じ実行体で 2 回ずつ交互)"
for round in 1 2; do
  for impl in core n3g; do
    exe=$CORE; [ $impl = n3g ] && exe=$N3G
    "$exe" --perf "$W/perf/ntfs_perf.img" /big.bin /many > "$W/perf.$impl.$round.txt" 2> "$W/perf.$impl.$round.err" || { cat "$W/perf.$impl.$round.err"; say "- FAIL: perf $impl"; fail=1; }
  done
done
for impl in core n3g; do
  for round in 1 2; do
    say "- $impl (run $round): $(tr '\n' ' ' < "$W/perf.$impl.$round.txt")"
  done
done

if [ $fail -ne 0 ]; then echo "FUSESHIM TESTS FAILED"; exit 1; fi
echo "FUSESHIM TESTS PASSED"
