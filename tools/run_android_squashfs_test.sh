#!/bin/bash
# squashfs の AVD 実機テスト: フィクスチャ (tools/gen_squashfs_fixture.py の出力) を端末へ push し、SquashfsInstrumentedTest を実行する。
# 使い方: tools/run_android_squashfs_test.sh <fixture_dir> [adb のパス]    ADB_SERIAL (既定 emulator-5556) の AVD にだけ触る。
# 先に ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest を済ませておく。flock で AVD の同時利用を避ける: flock -w 7200 /tmp/usbmem_avd.lock tools/run_android_squashfs_test.sh ...
set -u
FIX=${1:?fixture dir}
ADB=${2:-/run/media/u/disk31/android_home/platform-tools/adb}
SER=${ADB_SERIAL:-emulator-5556}
case "$SER" in emulator-*) ;; *) echo "refusing non-emulator serial $SER"; exit 2;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
adb() { "$ADB" -s "$SER" "$@"; }
for n in gzip_128k gzip_4k xz_128k zstd_128k lz4_128k lzo_128k nocomp; do
  for e in img manifest ref_1 ref_2; do adb push "$FIX/$n.$e" "/data/local/tmp/squashfs_$n.$e" >/dev/null || exit 1; done
done
adb push "$FIX/lzma_refused.img" /data/local/tmp/squashfs_lzma_refused.img >/dev/null || exit 1
adb install -r -t "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" >/dev/null || exit 1
adb install -r -t "$ROOT/android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk" >/dev/null || exit 1
OUT=$(adb shell am instrument -w -e class org.usbmem.app.SquashfsInstrumentedTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -25
adb shell 'rm -f /data/local/tmp/squashfs_*' >/dev/null
echo "$OUT" | grep -q "^OK (" || exit 1
