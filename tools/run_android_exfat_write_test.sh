#!/bin/sh
# Android エミュレータでの exFAT 書き込みテスト (実験的): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストの独立実装で検証。
#   検証: (1) fsck.exfat -n が clean かつ VolumeDirty なし (クリーンアンマウントされた) (2) 同じ操作列を exfat-fuse 上で POSIX 操作として実行した結果とツリー全体が一致
#         (partial_valid.bin だけは exfat-fuse が ValidDataLength の規則を守らないので、規則から計算した期待値 tools/exfat_vdl_expect.py と比べる)。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、フィクスチャ /dev/shm/usbmem_exfat_$USER/c4k.img (tools/gen_exfat_fixture.py)、exfatprogs / exfat-fuse。
# 終了時に Gradle デーモンを止める (メモリ節約)。使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_exfat_write_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_exfat_${USER:-u}}
for c in fsck.exfat mount.exfat-fuse fusermount3 python3 adb gcc; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/c4k.img" ] || { echo "skip: no fixture $D/c4k.img" >&2; exit 77; }
W=$(mktemp -d /dev/shm/usbmem_axw.XXXXXX)
cleanup() {
  fusermount3 -u "$W/mnt" 2>/dev/null || true; rm -rf "$W"
  adb shell rm -f /data/local/tmp/exfat_wr.img /data/local/tmp/exfat_wr_ops.txt /data/local/tmp/exfat_dirty.img 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
gcc -shared -fPIC -O1 -o "$W/shim.so" "$HERE/exfat_fuse_shim.c" -ldl || { echo "skip: cannot build the fusermount3 shim" >&2; exit 77; }
fmount() { mkdir -p "$2"; LD_PRELOAD="$W/shim.so" mount.exfat-fuse -o "$3" "$1" "$2" 2>/dev/null; }
python3 -I "$HERE/gen_exfat_ops.py" 4096 "$W/ops.txt" 60
cp "$D/c4k.img" "$W/wr.img"
cp "$D/c4k.img" "$W/dirty.img"
python3 -I -c "import struct,sys; f=open(sys.argv[1],'r+b'); f.seek(106); f.write(struct.pack('<H', 2)); f.close()" "$W/dirty.img"
adb push "$W/wr.img" /data/local/tmp/exfat_wr.img >/dev/null
adb push "$W/ops.txt" /data/local/tmp/exfat_wr_ops.txt >/dev/null
adb push "$W/dirty.img" /data/local/tmp/exfat_dirty.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/exfat_rw_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.ExfatWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
adb exec-out run-as org.usbmem.app cat cache/exfat_rw_result.img > "$W/r.img"
adb shell run-as org.usbmem.app rm -f cache/exfat_rw_result.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
fsck.exfat -n "$W/r.img" >"$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; echo "FAIL: fsck.exfat not clean"; exit 1; }
fl=$(python3 -I -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(106); print(struct.unpack('<H',f.read(2))[0])" "$W/r.img")
[ $((fl & 6)) -eq 0 ] || { echo "FAIL: VolumeFlags=$fl (VolumeDirty / MediaFailure set)"; exit 1; }
# 独立実装で同じ操作列を実行して比較
cp "$D/c4k.img" "$W/ref.img"
fmount "$W/ref.img" "$W/mnt" rw || { echo "FAIL: exfat-fuse rw mount"; exit 1; }
python3 -I "$HERE/ops_ref.py" "$W/mnt" "$W/ops.txt"; fusermount3 -u "$W/mnt"
tree_of() { fmount "$1" "$W/mnt" ro; python3 -I "$HERE/ops_ref.py" --tree "$W/mnt" > "$2"; fusermount3 -u "$W/mnt"; }
tree_of "$W/r.img" "$W/ta"; tree_of "$W/ref.img" "$W/tr"
python3 -I "$HERE/exfat_vdl_expect.py" > "$W/vdl.exp"; grep -F ' partial_valid.bin' "$W/ta" > "$W/vdl.got" || true
cmp -s "$W/vdl.exp" "$W/vdl.got" || { echo "FAIL: partial_valid.bin differs from the VDL rule"; exit 1; }
grep -v -F ' partial_valid.bin' "$W/ta" > "$W/ta2"; grep -v -F ' partial_valid.bin' "$W/tr" > "$W/tr2"
if cmp -s "$W/ta2" "$W/tr2"; then echo "android exfat write: $(wc -l < "$W/ta") entries identical to exfat-fuse reference, fsck.exfat clean, VolumeDirty cleared"
else diff "$W/ta2" "$W/tr2" | head -10; echo "FAIL: tree differs"; exit 1; fi
