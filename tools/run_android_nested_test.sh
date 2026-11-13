#!/bin/sh
# Android エミュレータでのネスト (ループバック) マウントのテスト: フィクスチャを送って計装テスト (NestedMountTest) を実行する。docs/nested-mount.md
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、
#       フィクスチャ /dev/shm/usbmem_nested_$USER (tools/gen_nested_fixture.py。無ければ作る)。
# 終了時に Gradle デーモンを止める (メモリ節約)。AVD は他と共有するので flock で直列化する。
# 使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_nested_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_nested_${USER:-u}}
command -v adb >/dev/null || { echo "skip: adb missing" >&2; exit 77; }
[ -f "$D/p_avd.img" ] || python3 -I "$HERE/gen_nested_fixture.py" "$D" >/dev/null
[ -f "$D/p_avd.img" ] || { echo "skip: no fixture $D/p_avd.img" >&2; exit 77; }
cleanup() {
  adb shell rm -f /data/local/tmp/p_avd.img /data/local/tmp/p_avd_rw.img /data/local/tmp/nested.manifest 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && nice -n 10 ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
APKD="$HERE/../android/app/build/outputs/apk"
exec 9>/tmp/usbmem_avd.lock
flock -w 7200 9
for f in p_avd.img p_avd_rw.img nested.manifest; do adb push "$D/$f" /data/local/tmp/$f >/dev/null; done
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
OUT=$(adb shell am instrument -w -e class org.usbmem.app.NestedMountTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -6
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -60; echo "FAIL: instrumentation"; exit 1; }
echo "OK: nested mount on the emulator"
