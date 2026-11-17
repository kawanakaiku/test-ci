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

compare_one() {  # name image manifest tag
  local name=$1 img=$2 man=$3 d=$W/cmp_$4
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
  compare_one "basic cluster=$cl" "$fx/ntfs_basic.img" "$fx/ntfs_basic.manifest" "basic_c$cl"
done

# ---- 1b. 別プロセス (socketpair の RPC) 越し: 同じ基本フィクスチャで、出力が同一プロセスの形と一致するか
d=$W/rpc_basic; mkdir -p "$d"
"$N3G" --rpc "$W/basic_c4096/ntfs_basic.img" > "$d/n3g_rpc.txt" 2> "$d/n3g_rpc.err"; rc=$?
say "### 別プロセス (RPC) 越しの ntfs-3g (basic cluster=4096)"
say "- rc=$rc $(tail -1 "$d/n3g_rpc.err")"
if [ $rc -ne 0 ] || ! diff -q <(strip_mtime "$d/n3g_rpc.txt") "$W/cmp_basic_c4096/n3g.nomtime" >/dev/null; then say "- FAIL: RPC 越しの出力が同一プロセスの出力と違う"; head -20 "$d/n3g_rpc.err"; fail=1; else say "- 同一プロセスの出力と一致"; fi

# ---- 2. 性能 (イメージは 1 つ。自前 → ntfs-3g を 2 回交互に)
sh "$HERE/gen_fuseshim_perf_fixture.sh" "$W/perf" > "$W/perf.gen.log" 2>&1 || { cat "$W/perf.gen.log"; echo "性能用フィクスチャ生成に失敗" >&2; exit 1; }
compare_one "perf image (全ツリー比較)" "$W/perf/ntfs_perf.img" "" perf
say "### 性能 (CI のマシンの値は参考。同じイメージ・同じ実行体で 2 回ずつ交互)"
for round in 1 2; do
  for impl in core n3g; do
    exe=$CORE; [ $impl = n3g ] && exe=$N3G
    "$exe" --perf "$W/perf/ntfs_perf.img" /big.bin /many > "$W/perf.$impl.$round.txt" 2> "$W/perf.$impl.$round.err" || { cat "$W/perf.$impl.$round.err"; say "- FAIL: perf $impl"; fail=1; }
  done
done
# 読み取りキャッシュ (64 MiB) を下位デバイスの前に入れた場合 (実機 USB のようにページキャッシュが無い状況の実験)
for impl in core n3g; do
  exe=$CORE; [ $impl = n3g ] && exe=$N3G
  FSDUMP_CACHE_MB=64 "$exe" --perf "$W/perf/ntfs_perf.img" /big.bin /many > "$W/perf.$impl.cache.txt" 2> "$W/perf.$impl.cache.err" || { cat "$W/perf.$impl.cache.err"; say "- FAIL: perf $impl (cache)"; fail=1; }
  say "- $impl (cache 64MiB): $(tr '\n' ' ' < "$W/perf.$impl.cache.txt")"
done
for impl in core n3g; do
  for round in 1 2; do
    say "- $impl (run $round): $(tr '\n' ' ' < "$W/perf.$impl.$round.txt")"
  done
done

# RPC 越し (fork した子プロセスに ntfs-3g。本体側は socketpair の RPC だけ)
for round in 1 2; do
  "$N3G" --rpc --perf "$W/perf/ntfs_perf.img" /big.bin /many > "$W/perf.n3grpc.$round.txt" 2> "$W/perf.n3grpc.$round.err" || { cat "$W/perf.n3grpc.$round.err"; say "- FAIL: perf n3g rpc"; fail=1; }
  say "- n3g + RPC (run $round): $(tr '\n' ' ' < "$W/perf.n3grpc.$round.txt")"
done

# ---- 3. wimlib の wimmount (shim の libfuse 3.x 系)。usbmem_fsdump_wim があるときだけ
WIMX=$B/usbmem_fsdump_wim
if [ -x "$WIMX" ]; then
  say "### wimlib wimmount (shim 越し、読み取り専用)"
  python3 -I "$HERE/fuseshim/gen_wim_fixture.py" "$W/wim" > "$W/wim.gen.log" 2>&1 || { cat "$W/wim.gen.log"; say "- FAIL: wim フィクスチャ生成"; fail=1; }
  strip_mtime "$W/wim/expected.txt" > "$W/wim/expected.nomtime"
  for v in lzx none lzms; do
    "$WIMX" "$W/wim/$v.wim" > "$W/wim/$v.txt" 2> "$W/wim/$v.err"; rc=$?
    say "- $v.wim: rc=$rc $(tail -1 "$W/wim/$v.err")"
    strip_mtime "$W/wim/$v.txt" > "$W/wim/$v.nomtime"
    if [ $rc -ne 0 ] || ! diff -u "$W/wim/expected.nomtime" "$W/wim/$v.nomtime" > "$W/wim/$v.diff"; then say "- FAIL: $v.wim が元のツリーと一致しない"; head -20 "$W/wim/$v.diff" | tee -a "$SUM"; head -20 "$W/wim/$v.err"; fail=1
    else say "  - 元のツリー ($(wc -l < "$W/wim/expected.nomtime") エントリ: 名前・種別・サイズ・sha256) と一致"; fi
  done
  for v in lzx none; do
    "$WIMX" --perf "$W/wim/$v.wim" /big.bin /many > "$W/wim/perf.$v.txt" 2> "$W/wim/perf.$v.err" || { cat "$W/wim/perf.$v.err"; say "- FAIL: wim perf $v"; fail=1; }
    say "- wim perf $v: $(tr '\n' ' ' < "$W/wim/perf.$v.txt")"
  done
  "$WIMX" --rpc "$W/wim/lzx.wim" > "$W/wim/lzx_rpc.txt" 2> "$W/wim/lzx_rpc.err" && diff -q <(strip_mtime "$W/wim/lzx_rpc.txt") "$W/wim/lzx.nomtime" >/dev/null && say "- RPC 越しの wimmount: 一致" || { say "- FAIL: RPC 越しの wimmount"; head -10 "$W/wim/lzx_rpc.err"; fail=1; }
fi

if [ $fail -ne 0 ]; then echo "FUSESHIM TESTS FAILED"; exit 1; fi
echo "FUSESHIM TESTS PASSED"
