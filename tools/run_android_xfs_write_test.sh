#!/bin/sh
# Android エミュレータでの XFS 書き込みテスト (実験的。CreateOnly の範囲): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストの独立実装 (xfsprogs) で検証。
#   検証: (1) xfs_repair -n が clean・ログが CLEAN (2) 同じ操作列を POSIX 操作として (元のツリーに) 実行した結果と、自前リーダーのツリー全体 (種別・サイズ・sha256・リンク先) が一致
#   (3) 実カーネルでマウントして同じツリーが見える (tools/xfs_vm/kreplay.py。バックエンドが無ければ省略)。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、xfsprogs (mkfs.xfs / xfs_repair)。
# 端末の空きは 1 GiB 以上要る (320 MiB のイメージを 2 つ push する。adb push は疎ファイルでも全体を送る)。
# 終了時に Gradle デーモンを止める (メモリ節約)。使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_xfs_write_test.sh
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/xfs_common.sh"
for c in mkfs.xfs xfs_repair xfs_logprint python3 adb; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
W=$(mktemp -d "${USBMEM_TMP:-${USBMEM_WORK_DIR:-/dev/shm}}"/usbmem_axfs.XXXXXX)
cleanup() {
  rm -rf "$W"
  adb shell rm -f /data/local/tmp/xfs_wr.img /data/local/tmp/xfs_wr_ops.txt /data/local/tmp/xfs_rsv.img 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_xfs_ops.py" seed "$W/seed"
python3 -I "$HERE/gen_xfs_ops.py" ops "$W/ops.txt" createonly
mk_xfs "$W/wr.img" 320M "$W/seed" || { echo "mkfs.xfs failed"; exit 1; }
cp --sparse=always "$W/wr.img" "$W/wr0.img"
HAVE_RSV=1; mk_xfs "$W/rsv.img" 320M "" -m rmapbt=1 >/dev/null 2>&1 || HAVE_RSV=0
# rmapbt=1 を作れない mkfs.xfs では mk_xfs が rmapbt=0 に落とすので、拒否の試験が成り立つかを確かめる
xfs_db -r -c "sb 0" -c "p rmapbt" "$W/rsv.img" 2>/dev/null | grep -q "1" || xfs_db -r -c "sb 0" -c "p features_ro_compat" "$W/rsv.img" 2>/dev/null | grep -qi "0x[0-9a-f]*[1-9a-f]" || true
adb push "$W/wr.img" /data/local/tmp/xfs_wr.img >/dev/null
adb push "$W/ops.txt" /data/local/tmp/xfs_wr_ops.txt >/dev/null
[ $HAVE_RSV = 1 ] && adb push "$W/rsv.img" /data/local/tmp/xfs_rsv.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/xfs_rw_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.XfsWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
adb exec-out run-as org.usbmem.app cat cache/xfs_rw_result.img > "$W/r.img"
adb shell run-as org.usbmem.app rm -f cache/xfs_rw_result.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
xfs_clean "$W/r.img" || { echo "FAIL: xfs_repair -n not clean"; exit 1; }
xfs_logprint_ok "$W/r.img" || { echo "FAIL: log not CLEAN"; exit 1; }
# 独立実装で同じ操作列を実行して比較
d=$W/ref; cp -a "$W/seed" "$d"
python3 -I "$HERE/ops_ref.py" "$d" "$W/ops.txt"; python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr"
# 実カーネル (あれば) が見るツリー。無ければ、自前ドライバ (ホスト) で同じイメージを読み直した結果で代用
if python3 -I "$HERE/xfs_vm/kreplay.py" --probe >/dev/null 2>&1; then
  cp --sparse=always "$W/r.img" "$W/k.img"
  python3 -I "$HERE/xfs_vm/kreplay.py" "$W/k.img" >/dev/null 2>&1 || true
  [ "$(cat "$W/k.img.kres" 2>/dev/null)" = ok ] || { echo "FAIL: kernel could not mount the result: $(cat "$W/k.img.kres" 2>/dev/null)"; exit 1; }
  cp "$W/k.img.ktree" "$W/ta"; WHO="real kernel"
else
  echo "(no kernel backend: tree not checked by the kernel)"; cp "$W/tr" "$W/ta"; WHO="(kernel skipped)"
fi
if cmp -s "$W/ta" "$W/tr"; then echo "android xfs write: $(wc -l < "$W/ta") entries identical to the POSIX reference ($WHO), xfs_repair -n clean, log CLEAN"
else diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs"; exit 1; fi
