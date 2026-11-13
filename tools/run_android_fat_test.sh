#!/bin/sh
# Android エミュレータでの FAT (fs/fat) 読み取りテスト: フィクスチャを adb push → ビルド → 計装テスト (FatInstrumentedTest)。
# 前提: ANDROID_SERIAL=emulator-XXXX (エミュレータのみ。実機は使わない)、android-sdk-exec (JDK + SDK) が使えること、フィクスチャ (tools/gen_fat_fixture.py)。
# 使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_fat_test.sh [フィクスチャ dir]
# 複数のエミュレータ作業が重ならないよう、呼び出し側で flock をかける: flock -w 7200 /tmp/usbmem_avd.lock tools/run_android_fat_test.sh
set -eu
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_fat_${USER:-u}}
SDKX=${ANDROID_SDK_EXEC:-/home/u/.local/bin/android-sdk-exec}
ADB="adb -s $ANDROID_SERIAL"
# AVD で使うのは小さめの 6 種 (4Kn セクタ・8K クラスタの大きいイメージはホストのテストで検証)
NAMES="fat12_f1440 fat12_c4k fat16_c512 fat16_c2k fat32_c512 fat32_c4k"
for n in $NAMES; do [ -f "$D/$n.img" ] || { echo "skip: no fixture $D/$n.img" >&2; exit 77; }; done
cleanup() {
  for n in $NAMES; do $ADB shell rm -f /data/local/tmp/fat_$n.img /data/local/tmp/fat_$n.manifest 2>/dev/null || true; done
}
trap cleanup EXIT
for n in $NAMES; do
  $ADB push "$D/$n.img" /data/local/tmp/fat_$n.img >/dev/null
  $ADB push "$D/$n.manifest" /data/local/tmp/fat_$n.manifest >/dev/null
done
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && "$SDKX" ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -q)
APKD="$HERE/../android/app/build/outputs/apk"
$ADB install -r -t "$APKD/debug/app-debug.apk" >/dev/null
$ADB install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
OUT=$($ADB shell am instrument -w -e class org.usbmem.app.FatInstrumentedTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -6
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -60; echo "FAIL: instrumentation"; exit 1; }
echo "android fat: OK"
