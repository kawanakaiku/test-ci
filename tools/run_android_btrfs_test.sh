#!/bin/bash
# Btrfs の AVD 実機テスト: フィクスチャ (tools/gen_btrfs_fixture.py の出力) を 1 つずつ端末へ送り、BtrfsInstrumentedTest の対応するテストを実行する。
# イメージは疎 (ほとんどゼロ) で 100 MB 前後あるので、gzip して送り、端末側で展開する (端末の空き容量を食わないよう 1 つ終わるごとに消す)。
# 使い方: tools/run_android_btrfs_test.sh <fixture_dir> [adb のパス]    ADB_SERIAL (既定 emulator-5556) の AVD にだけ触る。
# 先に ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest を済ませておく。flock で AVD の同時利用を避ける: flock -w 7200 /tmp/usbmem_avd.lock tools/run_android_btrfs_test.sh ...
set -u
FIX=${1:?fixture dir}
ADB=${2:-/run/media/u/disk31/android_home/platform-tools/adb}
SER=${ADB_SERIAL:-emulator-5556}
case "$SER" in emulator-*) ;; *) echo "refusing non-emulator serial $SER"; exit 2;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
adb() { "$ADB" -s "$SER" "$@"; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; adb shell "rm -f /data/local/tmp/btrfs_*" >/dev/null 2>&1' EXIT
adb install -r -t "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" >/dev/null || exit 1
adb install -r -t "$ROOT/android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk" >/dev/null || exit 1
FAIL=0
# 画像名:テストメソッド (カンマ区切りで複数のテストが同じ画像を使うものは 1 回の送信で済ませる)
run() {
  local name=$1; shift
  local methods=("$@")
  for e in manifest ref_1 ref_2; do [ -f "$FIX/$name.$e" ] && { adb push "$FIX/$name.$e" "/data/local/tmp/btrfs_$name.$e" >/dev/null || { FAIL=1; return 1; }; }; done
  gzip -1 -c "$FIX/$name.img" > "$TMP/$name.img.gz" || { FAIL=1; return 1; }
  adb push "$TMP/$name.img.gz" "/data/local/tmp/btrfs_$name.img.gz" >/dev/null || { FAIL=1; return 1; }
  adb shell "gunzip -c /data/local/tmp/btrfs_$name.img.gz > /data/local/tmp/btrfs_$name.img && rm -f /data/local/tmp/btrfs_$name.img.gz" || { FAIL=1; return 1; }
  rm -f "$TMP/$name.img.gz"
  local cls=""
  for m in "${methods[@]}"; do cls="${cls:+$cls,}org.usbmem.app.BtrfsInstrumentedTest#$m"; done
  local out
  out=$(adb shell am instrument -w -e class "$cls" org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
  echo "[$name] $(echo "$out" | grep -E '^(OK|FAILURES|Tests run)' | tr '\n' ' ')"
  echo "$out" | grep -q "^OK (" || { echo "$out" | tail -30; FAIL=1; }
  adb shell "rm -f /data/local/tmp/btrfs_$name.*" >/dev/null
}
run crc32c mountsAndVerifiesCrc32c unreplayedLogIsRefusedUnlessAllowedAndStaysReadOnly corruptMagicIsRefused
run xxhash mountsAndVerifiesXxhash
run sha256 mountsAndVerifiesSha256
run blake2 mountsAndVerifiesBlake2
run zlib mountsAndVerifiesZlib
run lzo mountsAndVerifiesLzo
run zstd mountsAndVerifiesZstd
run node4k mountsAndVerifiesNode4k
run node64k mountsAndVerifiesNode64k
run sector16k_lzo mountsAndVerifiesSector16kLzo
run sector64k_zlib mountsAndVerifiesSector64kZlib
run dup_dup mountsAndVerifiesDupDup
run mixed mountsAndVerifiesMixed
run noholes mountsAndVerifiesNoHoles
run subvols subvolumesAreTraversable
run multi_a multiDeviceIsRefused
exit $FAIL
