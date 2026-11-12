#!/bin/bash
# XFS の AVD 実機テスト: フィクスチャ (tools/gen_xfs_fixture.py の出力) を端末へ push し、XfsInstrumentedTest を実行する。
# 使い方: tools/run_android_xfs_test.sh <fixture_dir> [adb のパス]    ADB_SERIAL (既定 emulator-5556) の AVD にだけ触る。
# 先に ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest を済ませておく。flock で AVD の同時利用を避ける: flock -w 7200 /tmp/usbmem_avd.lock tools/run_android_xfs_test.sh ...
# イメージは mkfs.xfs の最小 (300 MiB) で、adb push は疎ファイルでも全体を送る。端末の空きは 1.5 GiB 以上要る (1 イメージずつ push して消す。dirty-log テストだけアプリのキャッシュへコピーする)。
set -u
FIX=${1:?fixture dir}
ADB=${2:-/run/media/u/disk31/android_home/platform-tools/adb}
SER=${ADB_SERIAL:-emulator-5556}
case "$SER" in emulator-*) ;; *) echo "refusing non-emulator serial $SER"; exit 2;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
adb() { "$ADB" -s "$SER" "$@"; }
adb install -r -t "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" >/dev/null || exit 1
adb install -r -t "$ROOT/android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk" >/dev/null || exit 1
rc=0
for n in v5_default v4_ftype0 v5_bs1k v5_nrext64 special_proto; do
  for e in img manifest ref_1 ref_2; do [ -f "$FIX/$n.$e" ] && { adb push "$FIX/$n.$e" "/data/local/tmp/xfs_$n.$e" >/dev/null || exit 1; }; done
  case $n in
    v5_default) M="mountsAndVerifiesV5Default dirtyLogIsRefusedUnlessAllowedAndNeverWritable" ;;
    v4_ftype0) M="mountsAndVerifiesV4NoFtype corruptMagicIsRefused" ;;
    v5_bs1k) M="mountsAndVerifiesBlock1kDir4k" ;;
    v5_nrext64) M="mountsAndVerifiesNrext64" ;;
    special_proto) M="mountsAndVerifiesSpecialFiles" ;;
  esac
  CL=""; for m in $M; do CL="${CL:+$CL,}org.usbmem.app.XfsInstrumentedTest#$m"; done
  OUT=$(adb shell am instrument -w -e class "$CL" org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
  echo "== $n"; echo "$OUT" | tail -12
  echo "$OUT" | grep -q "^OK (" || rc=1
  adb shell "rm -f /data/local/tmp/xfs_$n.*" >/dev/null
done
exit $rc
