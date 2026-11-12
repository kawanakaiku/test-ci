#!/bin/sh
# Android エミュレータでの ext4 書き込みテスト (fs/ext4 + fs/jbd2。実験的): 書き込み有効ビルド + 計装テスト Ext4WriteTest → 結果イメージを adb pull → ホストの独立実装で検証。
#   検証: (1) 操作列の結果: e2fsck -fn が clean かつ needs_recovery が消えている (クリーンにアンマウントされた)、同じ操作列を fuse2fs 上で実行した結果とツリー全体が一致。
#         (2) 未再生のジャーナル (debugfs が書いたトランザクション) を Android 上で再生した結果: e2fsck -fn が clean、再生されたブロックが e2fsck の再生結果と一致。
# 前提: エミュレータ起動済み (ANDROID_SERIAL=emulator-5556。実機は使わない)、フィクスチャ /dev/shm/usbmem_ext4_$USER/e4_def1k.img (tools/gen_ext4_fixture.py)、
# e2fsprogs / fuse2fs、JAVA_HOME / ANDROID_HOME。Gradle はデーモンなしで動かす。
# 使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_ext4_write_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_ext4_${USER:-u}}
ADB=${ADB:-adb}
for c in e2fsck debugfs fuse2fs fusermount3 python3 dumpe2fs; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
[ -f "$D/e4_def1k.img" ] || { echo "skip: no fixture $D/e4_def1k.img" >&2; exit 77; }
W=$(mktemp -d "${USBMEM_WORK_DIR:-/dev/shm}"/usbmem_ae4.XXXXXX)
cleanup() {
  fusermount3 -u "$W/mnt" 2>/dev/null || true; rm -rf "$W"
  $ADB shell rm -f /data/local/tmp/ext4_wr.img /data/local/tmp/ext4_wr_ops.txt /data/local/tmp/ext4_dirty.img /data/local/tmp/ext4_journal.img 2>/dev/null || true
}
trap cleanup EXIT
python3 -I "$HERE/gen_ext2_ops.py" 1024 "$W/ops.txt" 60
printf 'write /unwritten.bin 4096 5000 31\nwrite /frag_deep.bin 300000 9000 32\ntruncate /frag_deep.bin 200001\n' >> "$W/ops.txt"
cp "$D/e4_def1k.img" "$W/wr.img"
cp "$D/e4_def1k.img" "$W/dirty.img"; debugfs -w -R "ssv state 0" "$W/dirty.img" >/dev/null 2>&1
# 未再生のジャーナル (e2fsprogs の debugfs が書いたトランザクション 2 つ。空きブロックへ)
truncate -s 32M "$W/j.img"; mke2fs -q -t ext4 -b 4096 "$W/j.img"
python3 -I -c "import sys; [open(sys.argv[1]+n,'wb').write(bytes(((i*13+s*101)&255) for i in range(3*4096))) for n,s in (('/j1',1),('/j2',2))]" "$W"
printf 'jo -c\njw -b 8152,8153,8154 %s/j1\njw -b 8155,8153 %s/j2\njc\n' "$W" "$W" > "$W/jcmd"
debugfs -w -f "$W/jcmd" "$W/j.img" >/dev/null 2>&1
dumpe2fs -h "$W/j.img" 2>/dev/null | grep -q needs_recovery || { echo "FAIL: journal fixture has no needs_recovery"; exit 1; }
cp "$W/j.img" "$W/je.img"; e2fsck -fy "$W/je.img" >/dev/null 2>&1 || true
for f in wr.img:ext4_wr.img ops.txt:ext4_wr_ops.txt dirty.img:ext4_dirty.img j.img:ext4_journal.img; do $ADB push "$W/${f%%:*}" "/data/local/tmp/${f##*:}" >/dev/null; done
APKD="$HERE/../android/app/build/outputs/apk"
if [ "${SKIP_BUILD:-0}" != 1 ]; then
  (cd "$HERE/../android" && ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q; ./gradlew --stop >/dev/null 2>&1 || true)
fi
$ADB install -r -t "$APKD/debug/app-debug.apk" >/dev/null
$ADB install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
$ADB shell run-as org.usbmem.app rm -f cache/ext4_rw_result.img cache/ext4_replay_result.img 2>/dev/null || true
OUT=$($ADB shell am instrument -w -e class org.usbmem.app.Ext4WriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
printf '%s\n' "$OUT" | tail -4
printf '%s\n' "$OUT" | grep -q "^OK (" || { printf '%s\n' "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
$ADB exec-out run-as org.usbmem.app cat cache/ext4_rw_result.img > "$W/r.img"
$ADB exec-out run-as org.usbmem.app cat cache/ext4_replay_result.img > "$W/jr.img"
$ADB shell run-as org.usbmem.app rm -f cache/ext4_rw_result.img cache/ext4_replay_result.img
[ -s "$W/r.img" ] && [ -s "$W/jr.img" ] || { echo "FAIL: result image missing"; exit 1; }
e2fsck -fn "$W/r.img" >"$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; echo "FAIL: e2fsck not clean (ops)"; exit 1; }
dumpe2fs -h "$W/r.img" 2>/dev/null | grep -q needs_recovery && { echo "FAIL: needs_recovery still set after unmount"; exit 1; }
cp "$D/e4_def1k.img" "$W/ref.img"; mkdir -p "$W/mnt"
fuse2fs -o rw,fakeroot "$W/ref.img" "$W/mnt" >/dev/null 2>&1
python3 -I "$HERE/ops_ref.py" "$W/mnt" "$W/ops.txt"; fusermount3 -u "$W/mnt"
tree_of() { fuse2fs -o ro,fakeroot "$1" "$W/mnt" >/dev/null 2>&1; python3 -I "$HERE/ops_ref.py" --tree "$W/mnt" > "$2"; fusermount3 -u "$W/mnt"; }
tree_of "$W/r.img" "$W/ta"; tree_of "$W/ref.img" "$W/tr"
cmp -s "$W/ta" "$W/tr" || { diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs"; exit 1; }
e2fsck -fn "$W/jr.img" >"$W/fsck2.log" 2>&1 || { head -20 "$W/fsck2.log"; echo "FAIL: e2fsck not clean (replay)"; exit 1; }
bl() { for b in 8152 8153 8154 8155; do dd if="$1" bs=4096 skip=$b count=1 2>/dev/null | sha256sum | cut -c1-16; done | tr '\n' ' '; }
[ "$(bl "$W/jr.img")" = "$(bl "$W/je.img")" ] || { echo "FAIL: replayed blocks differ from e2fsck's replay"; exit 1; }
echo "android ext4 write: $(wc -l < "$W/ta") entries identical to fuse2fs reference, e2fsck clean, needs_recovery cleared; journal replay on Android matches e2fsck's replay (e2fsck clean)"
