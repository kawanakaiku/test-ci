#!/bin/sh
# Android エミュレータでの Btrfs 書き込みテスト (実験的): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストの独立実装 (btrfs-progs) で検証。
#   検証: (1) btrfs check --readonly --check-data-csum が clean (2) 同じ操作列を POSIX 操作として (btrfs restore で取り出した元のツリーに) 実行した結果と、
#   btrfs restore で取り出した結果のツリー全体 (種別・サイズ・sha256・リンク先) が一致。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、btrfs-progs (mkfs.btrfs / btrfs)。
# 終了時に Gradle デーモンを止める (メモリ節約)。使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_btrfs_write_test.sh
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd)
for c in mkfs.btrfs btrfs python3 adb; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
W=$(mktemp -d "${USBMEM_TMP:-${USBMEM_WORK_DIR:-/dev/shm}}"/usbmem_abtrfs.XXXXXX)
cleanup() {
  rm -rf "$W"
  adb shell rm -f /data/local/tmp/btrfs_wr.img /data/local/tmp/btrfs_wr_ops.txt /data/local/tmp/btrfs_sv.img 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_btrfs_ops.py" seed "$W/seed"
python3 -I "$HERE/gen_btrfs_ops.py" ops "$W/ops.txt" seeded
truncate -s 128M "$W/wr.img"; mkfs.btrfs -q -f --rootdir "$W/seed" "$W/wr.img" >/dev/null
mkdir -p "$W/sv/sub"; echo x > "$W/sv/sub/f"
# --subvol は新しい btrfs-progs だけ。無ければこのイメージは作らず、端末側のテストは assume で飛ばす
truncate -s 128M "$W/sv.img"; HAVE_SV=1; mkfs.btrfs -q -f --rootdir "$W/sv" --subvol rw:sub "$W/sv.img" >/dev/null 2>&1 || { HAVE_SV=0; echo "note: mkfs.btrfs lacks --subvol; subvolume refusal test skipped"; }
adb push "$W/wr.img" /data/local/tmp/btrfs_wr.img >/dev/null
adb push "$W/ops.txt" /data/local/tmp/btrfs_wr_ops.txt >/dev/null
[ $HAVE_SV = 1 ] && adb push "$W/sv.img" /data/local/tmp/btrfs_sv.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/btrfs_rw_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.BtrfsWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
adb exec-out run-as org.usbmem.app cat cache/btrfs_rw_result.img > "$W/r.img"
adb shell run-as org.usbmem.app rm -f cache/btrfs_rw_result.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
btrfs check --readonly --check-data-csum "$W/r.img" >"$W/fsck.log" 2>&1 || { head -30 "$W/fsck.log"; echo "FAIL: btrfs check not clean"; exit 1; }
grep -q "no error found" "$W/fsck.log" || { head -30 "$W/fsck.log"; echo "FAIL: btrfs check"; exit 1; }
# 独立実装で同じ操作列を実行して比較
tree_of() { d=$(mktemp -d "$W/rs.XXXXXX"); btrfs restore -S -i "$1" "$d" >/dev/null 2>&1; python3 -I "$HERE/ops_ref.py" --tree "$d" > "$2"; rm -rf "$d"; }
d=$W/ref; mkdir -p "$d"; btrfs restore -S -i "$W/wr.img" "$d" >/dev/null 2>&1
python3 -I "$HERE/ops_ref.py" "$d" "$W/ops.txt"; python3 -I "$HERE/ops_ref.py" --tree "$d" > "$W/tr"
tree_of "$W/r.img" "$W/ta"
if cmp -s "$W/ta" "$W/tr"; then echo "android btrfs write: $(wc -l < "$W/ta") entries identical to the POSIX reference, btrfs check clean"
else diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs"; exit 1; fi
