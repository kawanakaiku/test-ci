#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# SMB (SMBJ) を足したことによる release APK のサイズ増加を測る (docs/smb.md 11 章)。GitHub Actions の smb グループだけが呼ぶ。
#   1. この木の release APK (R8 あり・署名なし) を作る
#   2. 比較のため、環境変数で指定したコミット / ブランチの release APK も作る (指定が無い・取れないときは省く)
#        SMB_SIZE_BASE_REF = SMB を入れる直前のコミット (SMB なしの基準)。増加 = この木 - 基準
#        SMB_SIZE_SMB_REF  = SMB だけを入れた時点のコミット (R8 の追加の絞り込み・他の機能の追加を含まない。任意)。
#                            これと基準の差が「SMB 単独の素の増加」、これとこの木の差が「その後の変更」(R8 の絞り込みなど)
#      どちらも `git fetch --depth=1 origin <ref>` で取る (コミット SHA でもブランチ名でもよい)。取れなければ、その比較だけ省いて理由を出す。
#   3. apkanalyzer / zip の中身から、dex・ネイティブライブラリの大きさ (増加)、SMB 関係のパッケージ別の dex の大きさ (R8 のあと) を出す
# 結果は標準出力と $GITHUB_STEP_SUMMARY ($OUT/smb_apk_size.md) へ。ビルドの失敗 (R8 の欠けたクラスなど) はこのスクリプトの失敗にする。
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AA=${SDK_ROOT:-/usr/local/lib/android/sdk}/cmdline-tools/latest/bin/apkanalyzer
OUT=${CI_LOG_DIR:-$ROOT/ci-logs}; mkdir -p "$OUT"
GRADLE_OPTS_EXTRA=(-PusbmemNtfsWrite=true)
BASE_REF=${SMB_SIZE_BASE_REF:-}
SMB_REF=${SMB_SIZE_SMB_REF:-}
TMP=${RUNNER_TEMP:-/tmp}

build_release() {   # build_release <android ディレクトリ> <ログ>
  ( cd "$1" && "$ROOT/tools/ci/retry_gradle.sh" ./gradlew --no-daemon :app:assembleRelease "${GRADLE_OPTS_EXTRA[@]}" ) > "$2" 2>&1
}
apk_of() { ls "$1"/app/build/outputs/apk/release/*.apk 2>/dev/null | head -1; }

# fetch_tree <ref> <ディレクトリ>: ref を深さ 1 で取って worktree にする。ローカルに既にあれば、それを使う。失敗したら 1
fetch_tree() {
  local ref=$1 dir=$2 target=
  if git -C "$ROOT" cat-file -e "${ref}^{commit}" 2>/dev/null; then target=$ref
  elif git -C "$ROOT" fetch --depth=1 origin "$ref" >/dev/null 2>&1; then target=FETCH_HEAD
  else return 1; fi
  git -C "$ROOT" worktree add --detach "$dir" "$target" >/dev/null 2>&1
}

# measure <ラベル> <ref または "-" (この木)>: release APK を作って、サイズ・dex・ネイティブライブラリ・パッケージ別の dex を $OUT/size_<ラベル>.* に残す。作れなければ 1
measure() {
  local label=$1 ref=$2 dir apk
  if [ "$ref" = "-" ]; then dir=$ROOT
  else
    dir="$TMP/tree_$label"
    if ! fetch_tree "$ref" "$dir"; then echo "(${label}: $ref を取れない。この比較は省く)"; echo "fetch failed: $ref" > "$OUT/size_$label.err"; return 1; fi
  fi
  if ! build_release "$dir/android" "$OUT/size_${label}_build.log"; then
    tail -30 "$OUT/size_${label}_build.log"; echo "(${label}: release ビルドが失敗)"; echo "build failed" > "$OUT/size_$label.err"; return 1
  fi
  apk=$(apk_of "$dir/android"); [ -n "$apk" ] || { echo "no apk for $label" > "$OUT/size_$label.err"; return 1; }
  cp "$apk" "$OUT/size_$label.apk"
  # 中身の内訳: APK 全体 / dex (圧縮前・後) / ネイティブライブラリ (ABI ごと、圧縮前・後) / その他
  python3 -I - "$apk" > "$OUT/size_$label.json" <<'PY'
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
r = {"apk": 0, "dex": 0, "dex_c": 0, "lib": 0, "lib_c": 0, "res": 0, "res_c": 0, "other": 0, "other_c": 0, "abi": {}}
import os
r["apk"] = os.path.getsize(sys.argv[1])
for i in z.infolist():
    n = i.filename
    if n.startswith("classes") and n.endswith(".dex"): k = "dex"
    elif n.startswith("lib/") and n.endswith(".so"):
        k = "lib"; a = n.split("/")[1]; d = r["abi"].setdefault(a, [0, 0]); d[0] += i.file_size; d[1] += i.compress_size
    elif n.startswith("res/") or n == "resources.arsc": k = "res"
    else: k = "other"
    r[k] += i.file_size; r[k + "_c"] += i.compress_size
print(json.dumps(r))
PY
  # dex のパッケージ別 (クラスの大きさの合計。R8 のあと)。apkanalyzer の生の出力も残す (形式が想定と違っても後から読める)
  if [ -x "$AA" ]; then
    "$AA" dex packages --defined-only "$apk" > "$OUT/size_${label}_dexpkgs.txt" 2>/dev/null || true
    "$AA" apk download-size "$apk" > "$OUT/size_${label}_download.txt" 2>/dev/null || true
  fi
  [ "$ref" = "-" ] || git -C "$ROOT" worktree remove --force "$dir" >/dev/null 2>&1
  return 0
}

if ! measure head -; then tail -60 "$OUT/size_head_build.log" 2>/dev/null; echo "FAIL: release build (R8) of this tree"; exit 1; fi
grep -iE "^(warning|w:).*(smbj|hierynomus|bouncycastle|mbassador|slf4j)|Missing class" "$OUT/size_head_build.log" | head -20
[ -n "$BASE_REF" ] && measure base "$BASE_REF"
[ -n "$SMB_REF" ] && measure smb "$SMB_REF"
[ -z "$BASE_REF" ] && echo "(SMB_SIZE_BASE_REF が空: 比較用のビルドは省く)"

python3 -I - "$OUT" "$BASE_REF" "$SMB_REF" > "$OUT/smb_apk_size.md" <<'PY'
import json, os, re, sys
out, base_ref, smb_ref = sys.argv[1:4]
def load(l):
    p = os.path.join(out, "size_%s.json" % l)
    return json.load(open(p)) if os.path.exists(p) else None
def pkgs(l):
    """apkanalyzer dex packages の C (クラス) 行から、パッケージ接頭辞ごとにサイズ (バイト) を合計する"""
    p = os.path.join(out, "size_%s_dexpkgs.txt" % l)
    if not os.path.exists(p): return None
    # 上から順に最初に当たったものに数える (asn-one は smbj と同じ com.hierynomus. の下にあるので先に判定する)
    groups = [("asn-one (com.hierynomus.asn1)", "com.hierynomus.asn1."), ("smbj (com.hierynomus.* の残り)", "com.hierynomus."), ("bouncycastle (org.bouncycastle)", "org.bouncycastle."),
              ("mbassador (net.engio)", "net.engio."), ("slf4j (org.slf4j)", "org.slf4j.")]
    tot = {g[0]: 0 for g in groups}; cnt = {g[0]: 0 for g in groups}; mine = 0; allc = 0; ok = False
    for line in open(p, errors="replace"):
        f = line.split()
        if len(f) < 6 or f[0] != "C": continue
        ok = True
        try: sz = int(f[4])
        except ValueError: continue
        name = f[5]; allc += sz
        if name.startswith("org.usbmem.app.smb.") or name.startswith("org.usbmem.app.ui.Smb"): mine += sz
        for g, pre in groups:
            if name.startswith(pre): tot[g] += sz; cnt[g] += 1; break
    if not ok: return None
    tot["アプリ側の SMB コード (org.usbmem.app.smb / ui.Smb*)"] = mine
    tot["(全クラスの合計)"] = allc
    return tot, cnt
def mb(b): return "%.2f MB" % (b / 1048576.0)
def kb(b): return "%d KB" % round(b / 1024.0)
def diff(a, b):
    d = a - b
    return "%+.2f MB (%+.1f%%)" % (d / 1048576.0, 100.0 * d / b if b else 0)
h, b, s = load("head"), load("base"), load("smb")
print("### SMB (SMBJ) の APK サイズ (release / R8 / 署名なし、arm64-v8a + x86_64 の両方を含む)")
print()
print("- 比較元 (SMB を入れる直前) = `%s`、SMB だけを入れた時点 = `%s`" % (base_ref or "(指定なし)", smb_ref or "(指定なし)"))
print()
cols = [("この木", h)] + ([("SMB 単独", s)] if s else []) + ([("SMB なし (基準)", b)] if b else [])
print("| 項目 | " + " | ".join(c[0] for c in cols) + " |")
print("|---|" + "---|" * len(cols))
def row(name, key, fmt=mb):
    print("| " + name + " | " + " | ".join(fmt(c[1][key]) if c[1] else "-" for c in cols) + " |")
row("APK 全体", "apk"); row("dex (圧縮前)", "dex"); row("dex (APK 内)", "dex_c")
row("ネイティブライブラリ (圧縮前)", "lib"); row("ネイティブライブラリ (APK 内)", "lib_c")
row("リソース (APK 内)", "res_c"); row("その他 (APK 内)", "other_c")
print()
if b and h:
    print("**増加 (この木 - 基準)**: APK %s、dex %s、ネイティブライブラリ %s (APK 内)" % (diff(h["apk"], b["apk"]), diff(h["dex"], b["dex"]), diff(h["lib_c"], b["lib_c"])))
    print()
if b and s:
    print("**SMB 単独の増加 (SMB 単独 - 基準)**: APK %s、dex %s、ネイティブライブラリ %s (APK 内)" % (diff(s["apk"], b["apk"]), diff(s["dex"], b["dex"]), diff(s["lib_c"], b["lib_c"])))
    print()
if h and s:
    print("**SMB 単独 → この木**: APK %s、dex %s (R8 の絞り込み・他の機能の追加の合計)" % (diff(h["apk"], s["apk"]), diff(h["dex"], s["dex"])))
    print()
if h and h["abi"]:
    print("ネイティブライブラリの ABI 別 (この木、圧縮前 / APK 内): " + "、".join("%s %s / %s" % (a, mb(v[0]), mb(v[1])) for a, v in sorted(h["abi"].items())))
    print()
ph, ps = pkgs("head"), pkgs("smb")
if ph:
    print("dex のパッケージ別 (R8 のあと、クラスの推定サイズの合計。apkanalyzer dex packages):")
    print()
    print("| パッケージ | この木 (クラス数) | " + ("SMB 単独 |" if ps else ""))
    print("|---|---|" + ("---|" if ps else ""))
    for k, v in ph[0].items():
        c = ph[1].get(k)
        print("| %s | %s%s | %s" % (k, kb(v), (" (%d)" % c) if c is not None else "", (kb(ps[0].get(k, 0)) + " |") if ps else ""))
    print()
else:
    print("(apkanalyzer の dex packages を読めなかった。size_head_dexpkgs.txt を見る)")
    print()
for l in ("head", "base", "smb"):
    p = os.path.join(out, "size_%s_download.txt" % l)
    if os.path.exists(p): print("download-size (%s): %s バイト" % (l, open(p).read().strip()))
for l in ("base", "smb"):
    p = os.path.join(out, "size_%s.err" % l)
    if os.path.exists(p): print("(%s の測定を省いた: %s)" % (l, open(p).read().strip()))
PY
cat "$OUT/smb_apk_size.md" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
# 生のダンプ (dexpkgs / json) は ci-logs に残る。APK 本体は大きいので消す
rm -f "$OUT"/size_*.apk
exit 0
