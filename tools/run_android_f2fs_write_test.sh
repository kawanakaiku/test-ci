#!/bin/sh
# Android エミュレータでの F2FS 書き込みテスト (自前実装のチェックポイント方式。実験的): 書き込み有効ビルド + 計装テスト F2fsWriteTest → 結果イメージを adb pull → ホストの独立実装で検証。
#   検証: (1) 操作列の結果: fsck.f2fs --dry-run -f が clean。(2) 本物のカーネル (CI: sudo のループマウント / ローカル: QEMU VM) が読み取り専用でマウントしたツリー =
#         カーネルが同じ操作列を実行した結果のツリー。カーネルが無ければ、ホスト側の usbmem_f2fs_write_tests (環境変数 USBMEM_F2FS_WRITE_TESTS) で同じ操作列を実行した結果と比べる (どちらも無ければ fsck だけ)。
#         (3) 圧縮つきのイメージは書き込み可を要求しても読み取り専用に落ちる (計装テスト側で確認)。
# 前提: エミュレータ起動済み (ANDROID_SERIAL=emulator-5556。実機は使わない)、フィクスチャ <dir>/ext.img (tools/gen_f2fs_fixture.py <dir> ext)、f2fs-tools、JAVA_HOME / ANDROID_HOME。Gradle はデーモンなしで動かす。
# 使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_f2fs_write_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_f2fs_${USER:-u}}
ADB=${ADB:-adb}
for c in mkfs.f2fs fsck.f2fs python3; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/ext.img" ] || { echo "skip: no fixture $D/ext.img" >&2; exit 77; }
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_af2.XXXXXX)
. "$HERE/f2fs_kernel_lib.sh"
cleanup() {
  f2k_cleanup; rm -rf "$W"
  $ADB shell rm -f /data/local/tmp/f2fs_wr.img /data/local/tmp/f2fs_wr_ops.txt /data/local/tmp/f2fs_ro.img 2>/dev/null || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_ext2_ops.py" 4096 "$W/ops.txt" 60
cp "$D/ext.img" "$W/wr.img"
rm -f "$W/ro.img"; truncate -s 64M "$W/ro.img"; mkfs.f2fs -q -f -O extra_attr,inode_checksum,sb_checksum,inode_crtime,lost_found,compression "$W/ro.img" >/dev/null 2>&1
for f in wr.img:f2fs_wr.img ops.txt:f2fs_wr_ops.txt ro.img:f2fs_ro.img; do $ADB push "$W/${f%%:*}" "/data/local/tmp/${f##*:}" >/dev/null; done
APKD="$HERE/../android/app/build/outputs/apk"
if [ "${SKIP_BUILD:-${USBMEM_SKIP_BUILD:-0}}" != 1 ]; then
  (cd "$HERE/../android" && ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q; ./gradlew --stop >/dev/null 2>&1 || true)
fi
$ADB install -r -t "$APKD/debug/app-debug.apk" >/dev/null
$ADB install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
$ADB shell run-as org.usbmem.app rm -f cache/f2fs_rw_result.img 2>/dev/null || true
OUT=$($ADB shell am instrument -w -e class org.usbmem.app.F2fsWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
printf '%s\n' "$OUT" | tail -4
printf '%s\n' "$OUT" | grep -q "^OK (" || { printf '%s\n' "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
$ADB exec-out run-as org.usbmem.app cat cache/f2fs_rw_result.img > "$W/r.img"
$ADB shell run-as org.usbmem.app rm -f cache/f2fs_rw_result.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
fsck.f2fs --dry-run -f "$W/r.img" > "$W/fsck.log" 2>&1 && ! grep -q '\[Fail\|\[ASSERT\]\|\[FIX\]' "$W/fsck.log" || { grep '\[Fail\|\[ASSERT\]\|\[FIX\]' "$W/fsck.log" | head; echo "FAIL: fsck.f2fs not clean"; exit 1; }
how="fsck.f2fs clean"
if [ "$KMODE" != off ]; then
  ktree "$W/r.img" "$W/ta" || { echo "FAIL: kernel cannot mount the result"; exit 1; }
  cp "$D/ext.img" "$W/ref.img"
  ktree "$W/ref.img" "$W/tr" "$W/ops.txt" || { echo "FAIL: kernel cannot run the reference ops"; exit 1; }
  how="$how; kernel ($KMODE) tree identical to the kernel's own execution of the ops"
elif [ -n "${USBMEM_F2FS_WRITE_TESTS:-}" ] && [ -x "$USBMEM_F2FS_WRITE_TESTS" ]; then
  "$USBMEM_F2FS_WRITE_TESTS" --dump "$W/r.img" "$W/ta"
  cp "$D/ext.img" "$W/ref.img"; "$USBMEM_F2FS_WRITE_TESTS" --ops "$W/ref.img" "$W/ops.txt" >/dev/null 2>&1; "$USBMEM_F2FS_WRITE_TESTS" --dump "$W/ref.img" "$W/tr"
  how="$how; tree identical to the host driver's execution of the ops"
else
  echo "(no kernel / host driver available: tree comparison skipped)" >&2; cp /dev/null "$W/ta"; cp /dev/null "$W/tr"
fi
cmp -s "$W/ta" "$W/tr" || { diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs"; exit 1; }
echo "android f2fs write: $(wc -l < "$W/ta") entries; $how"
