#!/bin/sh
# Android エミュレータでの ext2 書き込みテスト (実験的): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストの独立実装で検証。
#   検証: (1) e2fsck -fn が clean かつ s_state が VALID_FS (クリーンアンマウントされた) (2) 同じ操作列を fuse2fs 上で POSIX 操作として実行した結果とツリー全体が一致。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、フィクスチャ /dev/shm/usbmem_ext2_$USER/b1k.img (tools/gen_ext2_fixture.py)、e2fsprogs / fuse2fs。
# 終了時に Gradle デーモンを止める (メモリ節約)。使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_ext2_write_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_ext2_${USER:-u}}
for c in e2fsck debugfs fuse2fs fusermount3 python3 adb; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/b1k.img" ] || { echo "skip: no fixture $D/b1k.img" >&2; exit 77; }
W=$(mktemp -d "${USBMEM_TMP:-/dev/shm}"/usbmem_ae2.XXXXXX)
cleanup() {
  fusermount3 -u "$W/mnt" 2>/dev/null || true; rm -rf "$W"
  adb shell rm -f /data/local/tmp/ext2_wr.img /data/local/tmp/ext2_wr_ops.txt /data/local/tmp/ext2_dirty.img /data/local/tmp/ext4_empty.img 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_ext2_ops.py" 1024 "$W/ops.txt" 60
cp "$D/b1k.img" "$W/wr.img"
cp "$D/b1k.img" "$W/dirty.img"; debugfs -w -R "ssv state 0" "$W/dirty.img" >/dev/null 2>&1
adb push "$W/wr.img" /data/local/tmp/ext2_wr.img >/dev/null
adb push "$W/ops.txt" /data/local/tmp/ext2_wr_ops.txt >/dev/null
adb push "$W/dirty.img" /data/local/tmp/ext2_dirty.img >/dev/null
[ -f "$D/ext4_empty.img" ] && adb push "$D/ext4_empty.img" /data/local/tmp/ext4_empty.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/ext2_rw_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.Ext2WriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
adb exec-out run-as org.usbmem.app cat cache/ext2_rw_result.img > "$W/r.img"
adb shell run-as org.usbmem.app rm -f cache/ext2_rw_result.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
e2fsck -fn "$W/r.img" >"$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; echo "FAIL: e2fsck not clean"; exit 1; }
st=$(python3 -I -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(1024+58); print(struct.unpack('<H',f.read(2))[0])" "$W/r.img")
[ "$st" = 1 ] || { echo "FAIL: s_state=$st (not VALID_FS)"; exit 1; }
# 独立実装で同じ操作列を実行して比較
cp "$D/b1k.img" "$W/ref.img"; mkdir -p "$W/mnt"
fuse2fs -o rw,fakeroot "$W/ref.img" "$W/mnt" || { echo "FAIL: fuse2fs mount"; exit 1; }
python3 -I "$HERE/ops_ref.py" "$W/mnt" "$W/ops.txt"; fusermount3 -u "$W/mnt"
tree_of() { fuse2fs -o ro,fakeroot "$1" "$W/mnt" 2>/dev/null; python3 -I "$HERE/ops_ref.py" --tree "$W/mnt" > "$2"; fusermount3 -u "$W/mnt"; }
tree_of "$W/r.img" "$W/ta"; tree_of "$W/ref.img" "$W/tr"
if cmp -s "$W/ta" "$W/tr"; then echo "android ext2 write: $(wc -l < "$W/ta") entries identical to fuse2fs reference, e2fsck clean, state VALID_FS"
else diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs"; exit 1; fi
