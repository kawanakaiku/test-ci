#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# SMB (SMBJ) を足したことによる release APK のサイズ増加を測る (docs/smb.md)。GitHub Actions の smb グループだけが呼ぶ。
#   1. この木の release APK (R8 あり・署名なし) を作る
#   2. 比較のため、origin/main の release APK も作る (取れなければ省く)
#   3. apkanalyzer で、SMB 関係のパッケージが dex に占める大きさ (R8 のあと) を出す
# 結果は標準出力と $GITHUB_STEP_SUMMARY へ。ビルドの失敗 (R8 の欠けたクラスなど) はこのスクリプトの失敗にする。
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AA=${SDK_ROOT:-/usr/local/lib/android/sdk}/cmdline-tools/latest/bin/apkanalyzer
OUT=${CI_LOG_DIR:-$ROOT/ci-logs}; mkdir -p "$OUT"
GRADLE_OPTS_EXTRA=(-PusbmemNtfsWrite=true)

build_release() {   # build_release <android ディレクトリ> <ログ>
  ( cd "$1" && "$ROOT/tools/ci/retry_gradle.sh" ./gradlew --no-daemon :app:assembleRelease "${GRADLE_OPTS_EXTRA[@]}" ) > "$2" 2>&1
}
apk_of() { ls "$1"/app/build/outputs/apk/release/*.apk 2>/dev/null | head -1; }

build_release "$ROOT/android" "$OUT/size_head_build.log" || { tail -60 "$OUT/size_head_build.log"; echo "FAIL: release build (R8) of this tree"; exit 1; }
grep -iE "^(warning|w:).*(smbj|hierynomus|bouncycastle|mbassador|slf4j)|Missing class" "$OUT/size_head_build.log" | head -20
head_apk=$(apk_of "$ROOT/android"); [ -n "$head_apk" ] || { echo "FAIL: no release apk"; exit 1; }
head_size=$(stat -c %s "$head_apk")

base_size=""
BASE_DIR="${RUNNER_TEMP:-/tmp}/base_tree"
if git -C "$ROOT" fetch --depth=1 origin main >/dev/null 2>&1 && git -C "$ROOT" worktree add --detach "$BASE_DIR" FETCH_HEAD >/dev/null 2>&1; then
  if build_release "$BASE_DIR/android" "$OUT/size_base_build.log"; then
    base_apk=$(apk_of "$BASE_DIR/android"); [ -n "$base_apk" ] && base_size=$(stat -c %s "$base_apk")
  else tail -20 "$OUT/size_base_build.log"; fi
else echo "(origin/main を取れないので、比較用のビルドは省く)"; fi

mb() { awk -v b="$1" 'BEGIN{printf "%.2f", b/1048576}'; }
{
  echo "### SMB (SMBJ) の APK サイズ (release / R8 / 署名なし、arm64-v8a + x86_64 の両方を含む)"
  echo
  echo "| 項目 | サイズ |"; echo "|---|---|"
  echo "| この木の release APK | $(mb "$head_size") MB ($head_size B) |"
  if [ -n "$base_size" ]; then
    echo "| main の release APK | $(mb "$base_size") MB ($base_size B) |"
    echo "| 増加 | $(mb $((head_size - base_size))) MB ($((head_size - base_size)) B) |"
  fi
  if [ -x "$AA" ]; then
    echo; echo "dex のパッケージ別 (R8 のあと、推定サイズ。apkanalyzer dex packages):"; echo
    echo '```'
    "$AA" dex packages --defined-only "$head_apk" 2>/dev/null | grep -E '^P' | grep -E "com\.hierynomus|org\.bouncycastle|net\.engio|org\.slf4j|org\.usbmem\.app\.smb|com\.rcw|org\.ietf" | head -60
    echo '```'
    "$AA" apk download-size "$head_apk" 2>/dev/null | sed 's/^/download-size: /'
  else echo "(apkanalyzer が無い)"; fi
} | tee "$OUT/smb_apk_size.md" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
[ -d "$BASE_DIR" ] && git -C "$ROOT" worktree remove --force "$BASE_DIR" >/dev/null 2>&1
exit 0
