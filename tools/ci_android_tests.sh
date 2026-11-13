#!/bin/bash
# CI 用のまとめスクリプト (.github/workflows/android-emulator.yml から呼ばれる。docs/ci.md)。AVD (emulator-*) だけに触る。
#   tools/ci_android_tests.sh gen <グループ>...   ホストでフィクスチャを作る (エミュレータ不要。apt のホストツールが要る)
#   tools/ci_android_tests.sh run <グループ>...   エミュレータ上で FS 別の計装テストを順に実行し、失敗を集計する (1 つでも失敗なら終了コード 1)
# グループ (workflow の matrix と同じ名前):
#   ext-fat        FAT (6 種) / ext2 書き込み / exFAT 書き込み
#   btrfs-xfs      Btrfs (16 イメージ) / XFS (5 イメージ)
#   squash-erofs   squashfs / erofs / F2FS (mkfs.f2fs + sload.f2fs で作る 1 イメージだけ。カーネル製 k_* は VM が要るので CI では扱わない)
#   nested-ntfs    ネスト (ループバック) マウント / NTFS 書き込み (SAF)
# 環境変数: ANDROID_SERIAL (既定 emulator-5554。emulator-* 以外は拒否)、CI_FIX (フィクスチャ置き場。既定 ${RUNNER_TEMP:-/tmp}/usbmem_fix)、
#          CI_LOG_DIR (ログ。既定 <リポジトリ>/ci-logs)。run は ビルド済みの APK (-PusbmemNtfsWrite=true のビルド) が android/app/build/outputs/apk にあること。
# 既存の tools/run_android_*_test.sh をそのまま呼ぶ (USBMEM_SKIP_BUILD=1 でスクリプト内のビルドを省く)。
# スクリプトがフィクスチャ無し・ツールなしで「skip (終了コード 77)」と言った場合も CI では失敗として数える (黙って通さない)。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
MODE=${1:?usage: ci_android_tests.sh gen|run <group>...}; shift
[ $# -ge 1 ] || { echo "usage: ci_android_tests.sh $MODE <group>..." >&2; exit 2; }
export PATH="$PATH:/sbin:/usr/sbin:${ANDROID_HOME:+$ANDROID_HOME/platform-tools}"
FIX=${CI_FIX:-${RUNNER_TEMP:-/tmp}/usbmem_fix}
LOG=${CI_LOG_DIR:-$ROOT/ci-logs}
mkdir -p "$FIX" "$LOG"

export ANDROID_SERIAL=${ANDROID_SERIAL:-${ADB_SERIAL:-emulator-5554}}
export ADB_SERIAL=$ANDROID_SERIAL
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator ($ANDROID_SERIAL)" >&2; exit 2;; esac
export USBMEM_SKIP_BUILD=1
export ANDROID_SDK_EXEC=/usr/bin/env      # run_android_fat_test.sh の android-sdk-exec の代わり (ビルドは省くので実際には使わない)

# ---- フィクスチャ生成 -------------------------------------------------------------
py() { python3 -I "$@"; }
gen_fs() {
  case $1 in
    fat)         py "$HERE/gen_fat_fixture.py" "$FIX/fat" ;;
    ext2_write)  py "$HERE/gen_ext2_fixture.py" "$FIX/ext2" ;;
    exfat_write) py "$HERE/gen_exfat_fixture.py" "$FIX/exfat" ;;
    btrfs)       py "$HERE/gen_btrfs_fixture.py" "$FIX/btrfs" ;;
    xfs)         py "$HERE/gen_xfs_fixture.py" "$FIX/xfs" ;;
    squashfs)    py "$HERE/gen_squashfs_fixture.py" "$FIX/squashfs" ;;
    erofs)       py "$HERE/gen_erofs_fixture.py" "$FIX/erofs" ;;
    f2fs)        py "$HERE/gen_f2fs_fixture.py" "$FIX/f2fs" ext ;;
    nested)      py "$HERE/gen_nested_fixture.py" "$FIX/nested" ;;
    write) bash "$HERE/gen_ntfs_fixture.sh" "$FIX/ntfs" ;;
    *) echo "unknown fs $1" >&2; return 2 ;;
  esac
}

# ---- テスト実行 -------------------------------------------------------------------
ADB=$(command -v adb || true)
adbs() { "$ADB" -s "$ANDROID_SERIAL" "$@"; }
APKD="$ROOT/android/app/build/outputs/apk"
RUNNER=org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner

# F2FS: 全部入りのスクリプト (run_android_f2fs_test.sh) は k_* (QEMU VM が作るカーネル製イメージ) を要求するので、ここでは ext だけを送って 1 テストを実行する
run_f2fs() {
  local d=$FIX/f2fs tmp; tmp=$(mktemp -d)
  adbs install -r -t "$APKD/debug/app-debug.apk" >/dev/null || return 1
  adbs install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null || return 1
  gzip -1 -c "$d/ext.img" > "$tmp/ext.img.gz" || return 1
  adbs push "$tmp/ext.img.gz" /data/local/tmp/f2fs_ext.img.gz >/dev/null || return 1
  for e in manifest ref_1 ref_2; do adbs push "$d/ext.$e" "/data/local/tmp/f2fs_ext.$e" >/dev/null || return 1; done
  rm -rf "$tmp"
  local out; out=$(adbs shell am instrument -w -e class org.usbmem.app.F2fsInstrumentedTest#mountsAndVerifiesMkfsSload "$RUNNER")
  echo "$out" | tail -25
  adbs shell 'rm -f /data/local/tmp/f2fs_*' >/dev/null
  echo "$out" | grep -q "^OK (" || return 1
}

run_fs() {
  case $1 in
    fat) bash "$HERE/run_android_fat_test.sh" "$FIX/fat" ;;
    ext2_write) bash "$HERE/run_android_ext2_write_test.sh" "$FIX/ext2" ;;
    exfat_write) bash "$HERE/run_android_exfat_write_test.sh" "$FIX/exfat" ;;
    btrfs)       bash "$HERE/run_android_btrfs_test.sh" "$FIX/btrfs" "$ADB" ;;
    xfs)         bash "$HERE/run_android_xfs_test.sh" "$FIX/xfs" "$ADB" ;;
    squashfs)    bash "$HERE/run_android_squashfs_test.sh" "$FIX/squashfs" "$ADB" ;;
    erofs)       bash "$HERE/run_android_erofs_test.sh" "$FIX/erofs" "$ADB" ;;
    f2fs)        run_f2fs ;;
    nested) bash "$HERE/run_android_nested_test.sh" "$FIX/nested" ;;
    write) bash "$HERE/run_android_write_test.sh" "$FIX/ntfs" ;;
    *) echo "unknown fs $1" >&2; return 2 ;;
  esac
}

group_fs() {
  case $1 in
    ext-fat)       echo "fat ext2_write exfat_write" ;;
    btrfs-xfs)     echo "btrfs xfs" ;;
    squash-erofs)  echo "squashfs erofs f2fs" ;;
    nested-ntfs)   echo "nested write" ;;
    *) echo "unknown group $1" >&2; return 2 ;;
  esac
}

FSS=()
for g in "$@"; do
  l=$(group_fs "$g") || exit 2
  # shellcheck disable=SC2206
  FSS+=($l)
done

if [ "$MODE" = gen ]; then
  rc=0
  for fs in "${FSS[@]}"; do
    echo "::group::gen $fs"
    gen_fs "$fs" > "$LOG/gen_$fs.log" 2>&1; r=$?
    tail -15 "$LOG/gen_$fs.log"
    echo "::endgroup::"
    [ $r -eq 0 ] || { echo "FAIL: fixture generation for $fs (rc=$r)"; rc=1; }
  done
  du -sh "$FIX"/* 2>/dev/null
  exit $rc
fi
[ "$MODE" = run ] || { echo "unknown mode $MODE" >&2; exit 2; }

[ -n "$ADB" ] || { echo "adb not found" >&2; exit 2; }
adbs wait-for-device
echo "device: $(adbs shell getprop ro.product.model | tr -d '\r') API $(adbs shell getprop ro.build.version.sdk | tr -d '\r') ($ANDROID_SERIAL)"
adbs shell df -h /data | tee "$LOG/df_start.txt"
adbs logcat -c
adbs logcat -v threadtime > "$LOG/logcat.txt" 2>&1 &
LOGCAT_PID=$!
cleanup() { kill "$LOGCAT_PID" 2>/dev/null; true; }
trap cleanup EXIT

rc=0; SUMMARY=()
for fs in "${FSS[@]}"; do
  echo "::group::android $fs"
  t0=$(date +%s)
  run_fs "$fs" > "$LOG/run_$fs.log" 2>&1; r=$?
  cat "$LOG/run_$fs.log"
  echo "::endgroup::"
  t1=$(date +%s)
  n=$(grep -oE 'OK \(([0-9]+) tests?\)' "$LOG/run_$fs.log" | grep -oE '[0-9]+' | awk '{s+=$1} END{print s+0}')
  n=${n:-0}
  if [ $r -eq 0 ] && [ "$n" -gt 0 ]; then st=PASS
  elif [ $r -eq 0 ]; then st="FAIL (テスト 0 件)"; rc=1
  elif [ $r -eq 77 ]; then st="FAIL (skip 77: フィクスチャ / ツールなし)"; rc=1
  else st="FAIL (rc=$r)"; rc=1; fi
  [ "$st" = PASS ] || echo "::error::android $fs: $st"
  SUMMARY+=("| $fs | $st | $n | $((t1 - t0)) s |")
  adbs shell df -h /data | tail -1
done

{
  echo "### Android 計装テスト ($*)"
  echo
  echo "| FS | 結果 | テスト件数 (OK の合計) | 時間 |"
  echo "|---|---|---|---|"
  printf '%s\n' "${SUMMARY[@]}"
} | tee "$LOG/summary.md" > "${GITHUB_STEP_SUMMARY:-/dev/null}"
cleanup
exit $rc
