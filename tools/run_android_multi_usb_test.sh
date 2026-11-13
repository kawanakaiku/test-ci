#!/bin/sh
# Android エミュレータでの「複数 USB を同時に開く」ときのボリューム登録簿・DocumentsProvider のテスト (計装テスト MultiUsbTest)。docs/multi-usb.md
# USB の実機・AVD での USB 転送は無いので、デバイスは「偽のセッション + 実イメージのボリューム」で表す。フィクスチャはネストのテストと同じ (p_avd.img, nested.manifest)。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、
#       フィクスチャ /dev/shm/usbmem_nested_$USER (tools/gen_nested_fixture.py。無ければ作る)。
# 終了時に Gradle デーモンを止める。AVD は他と共有するので、この中で flock を取る (外側で flock しないこと。nested と同じ)。
# 使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_multi_usb_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_nested_${USER:-u}}
command -v adb >/dev/null || { echo "skip: adb missing" >&2; exit 77; }
[ -f "$D/p_avd.img" ] || python3 -I "$HERE/gen_nested_fixture.py" "$D" >/dev/null
[ -f "$D/p_avd.img" ] || { echo "skip: no fixture $D/p_avd.img" >&2; exit 77; }
cleanup() {
  adb shell rm -f /data/local/tmp/p_avd.img /data/local/tmp/nested.manifest 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
(cd "$HERE/../android" && nice -n 10 ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
APKD="$HERE/../android/app/build/outputs/apk"
exec 9>/tmp/usbmem_avd.lock
flock -w 7200 9
for f in p_avd.img nested.manifest; do adb push "$D/$f" /data/local/tmp/$f >/dev/null; done
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
OUT=$(adb shell am instrument -w -e class org.usbmem.app.MultiUsbTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -6
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -60; echo "FAIL: instrumentation"; exit 1; }
echo "OK: multi-usb registry/provider on the emulator"
