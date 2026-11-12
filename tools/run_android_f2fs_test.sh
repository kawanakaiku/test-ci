#!/bin/bash
# F2FS の AVD 実機テスト: フィクスチャ (tools/gen_f2fs_fixture.py と tools/f2fs_vm/gen_k_fixtures.py の出力) を gzip して端末へ push し、F2fsInstrumentedTest を実行する。
# 使い方: tools/run_android_f2fs_test.sh <fixture_dir> [adb のパス]    ADB_SERIAL (既定 emulator-5556) の AVD にだけ触る。
# 先に ./gradlew --no-daemon :app:assembleDebug :app:assembleDebugAndroidTest を済ませておく。flock で AVD の同時利用を避ける: flock -w 7200 /tmp/usbmem_avd.lock tools/run_android_f2fs_test.sh ...
set -u
FIX=${1:?fixture dir}
ADB=${2:-/run/media/u/disk31/android_home/platform-tools/adb}
SER=${ADB_SERIAL:-emulator-5556}
case "$SER" in emulator-*) ;; *) echo "refusing non-emulator serial $SER"; exit 2;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
adb() { "$ADB" -s "$SER" "$@"; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for n in ext k_basic k_nofeat k_dirty k_comp_lz4_c2 k_comp_lz4_c4 k_comp_lzo_c2 k_comp_lzorle_c3 k_comp_zstd_c2 k_unclean; do
  [ -f "$FIX/$n.img" ] || { echo "missing $FIX/$n.img"; exit 1; }
  gzip -1 -c "$FIX/$n.img" > "$TMP/$n.img.gz" || exit 1    # ゼロの多いイメージなので小さくなる
  adb push "$TMP/$n.img.gz" "/data/local/tmp/f2fs_$n.img.gz" >/dev/null || exit 1
  rm -f "$TMP/$n.img.gz"
  for e in manifest ref_1 ref_2; do adb push "$FIX/$n.$e" "/data/local/tmp/f2fs_$n.$e" >/dev/null || exit 1; done
done
adb install -r -t "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" >/dev/null || exit 1
adb install -r -t "$ROOT/android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk" >/dev/null || exit 1
OUT=$(adb shell am instrument -w -e class org.usbmem.app.F2fsInstrumentedTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -25
adb shell 'rm -f /data/local/tmp/f2fs_*' >/dev/null
echo "$OUT" | grep -q "^OK (" || exit 1
