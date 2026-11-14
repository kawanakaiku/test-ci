#!/bin/sh
# Android エミュレータでの XFS 書き込みテスト (実験的。CreateOnly の範囲 + Full の操作): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストの独立実装 (xfsprogs) で検証。
#   検証: (1) xfs_repair -n が clean・ログが CLEAN (2) 同じ操作列を POSIX 操作として (元のツリーに) 実行した結果と、自前リーダーのツリー全体 (種別・サイズ・sha256・リンク先) が一致
#   (3) 実カーネルでマウントして同じツリーが見える (tools/xfs_vm/kreplay.py。バックエンドが無ければ省略)。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、xfsprogs (mkfs.xfs / xfs_repair)。
# 端末の空きは 1.5 GiB 以上要る (320 MiB のイメージを 3 つ push する。adb push は疎ファイルでも全体を送る)。
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
  adb shell rm -f /data/local/tmp/xfs_wr.img /data/local/tmp/xfs_wr_ops.txt /data/local/tmp/xfs_wrf.img /data/local/tmp/xfs_wrf_ops.txt /data/local/tmp/xfs_rsv.img 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_xfs_ops.py" seed "$W/seed"
python3 -I "$HERE/gen_xfs_ops.py" ops "$W/ops.txt" createonly
python3 -I "$HERE/gen_xfs_ops.py" ops "$W/opsf.txt" seeded   # Full の操作 (unlink / rmdir / rename / truncate / link) を含む列
mk_xfs "$W/wr.img" 320M "$W/seed" || { echo "mkfs.xfs failed"; exit 1; }
cp --sparse=always "$W/wr.img" "$W/wrf.img"
HAVE_RSV=1; mk_xfs_plain "$W/rsv.img" 320M "" -m rmapbt=1 >/dev/null 2>&1 || HAVE_RSV=0   # 既定の rmapbt=0 を付けずに作る (書き込み拒否のイメージ)
adb push "$W/wr.img" /data/local/tmp/xfs_wr.img >/dev/null
adb push "$W/ops.txt" /data/local/tmp/xfs_wr_ops.txt >/dev/null
adb push "$W/wrf.img" /data/local/tmp/xfs_wrf.img >/dev/null
adb push "$W/opsf.txt" /data/local/tmp/xfs_wrf_ops.txt >/dev/null
[ $HAVE_RSV = 1 ] && adb push "$W/rsv.img" /data/local/tmp/xfs_rsv.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/xfs_rw_result.img cache/xfs_rwf_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.XfsWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
verify_result() {  # verify_result <android 側のファイル名> <操作列> <ラベル>
  adb exec-out run-as org.usbmem.app cat "cache/$1" > "$W/r.img"
  adb shell run-as org.usbmem.app rm -f "cache/$1"
  [ -s "$W/r.img" ] || { echo "FAIL: result image missing ($3)"; exit 1; }
  xfs_clean "$W/r.img" || { echo "FAIL: xfs_repair -n not clean ($3)"; exit 1; }
  xfs_logprint_ok "$W/r.img" || { echo "FAIL: log not CLEAN ($3)"; exit 1; }
  # 独立実装で同じ操作列を実行して比較
  d=$W/ref; rm -rf "$d"; cp -a "$W/seed" "$d"
  python3 -I "$HERE/ops_ref.py" "$d" "$2"; python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr"
  # 実カーネル (あれば) が見るツリー。無ければ、自前ドライバ (ホスト) で同じイメージを読み直した結果で代用
  if python3 -I "$HERE/xfs_vm/kreplay.py" --probe >/dev/null 2>&1; then
    cp --sparse=always "$W/r.img" "$W/k.img"
    python3 -I "$HERE/xfs_vm/kreplay.py" "$W/k.img" >/dev/null 2>&1 || true
    [ "$(cat "$W/k.img.kres" 2>/dev/null)" = ok ] || { echo "FAIL: kernel could not mount the result ($3): $(cat "$W/k.img.kres" 2>/dev/null)"; exit 1; }
    cp "$W/k.img.ktree" "$W/ta"; WHO="real kernel"
  else
    echo "(no kernel backend: tree not checked by the kernel)"; cp "$W/tr" "$W/ta"; WHO="(kernel skipped)"
  fi
  if cmp -s "$W/ta" "$W/tr"; then echo "android xfs write ($3): $(wc -l < "$W/ta") entries identical to the POSIX reference ($WHO), xfs_repair -n clean, log CLEAN"
  else diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs ($3)"; exit 1; fi
}
verify_result xfs_rw_result.img "$W/ops.txt" "CreateOnly"
verify_result xfs_rwf_result.img "$W/opsf.txt" "Full"
