#!/bin/sh
# Android エミュレータでの SAF 書き込みテスト (実験的): 書き込み有効ビルド + 計装テスト → 結果イメージを adb pull → ホストで独立検証。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL)、フィクスチャ /dev/shm/usbmem_ntfs_$USER/ntfs_basic.img
set -eu
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_ntfs_${USER:-u}}
W=$(mktemp -d /dev/shm/usbmem_aw.XXXXXX); trap 'fusermount3 -u "$W/m" 2>/dev/null || true; rm -rf "$W"' EXIT
adb push "$D/ntfs_basic.img" /data/local/tmp/ntfs_basic.img >/dev/null
adb shell run-as org.usbmem.app rm -f cache/rw_result.img 2>/dev/null || true
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
adb shell run-as org.usbmem.app rm -f cache/rw_result.img 2>/dev/null || true
OUT=$(adb shell am instrument -w -e class org.usbmem.app.DocumentsWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
adb exec-out run-as org.usbmem.app cat cache/rw_result.img > "$W/r.img"
adb shell run-as org.usbmem.app rm -f cache/rw_result.img; adb shell rm -f /data/local/tmp/ntfs_basic.img
[ -s "$W/r.img" ] || { echo "FAIL: result image missing"; exit 1; }
ntfsfix -n "$W/r.img" >/dev/null 2>&1 || { echo "FAIL ntfsfix"; exit 1; }
mkdir -p "$W/m"; ntfs-3g -o ro "$W/r.img" "$W/m"
python3 -I - "$W/m" "$D/ntfs_basic.manifest" <<'PY'
import sys, os, hashlib
root, man = sys.argv[1:3]
exp = {}
for l in open(man, encoding='utf-8'):
    k, sz, h, p = l.rstrip('\n').split(' ', 3); exp[p] = (k, sz, h)
# テストが行った変更を期待値へ反映
def pat(n, seed): return bytes(((i * 7) + seed) & 255 for i in range(n))
def put(path, data): exp[path] = ("f", str(len(data)), hashlib.sha256(data).hexdigest())
put("a.txt", b"replaced by SAF\n")
for k in [k for k in exp if k == "dir1" or k.startswith("dir1/") or k == "many" or k.startswith("many/")]: exp.pop(k)   # 中身ごと削除
keep = bytearray(pat(500_000, 1)); keep[100_000:104_096] = bytes([0xAB]) * 4096
put("saf_keep.bin", bytes(keep))
put("big.bin", pat(200_000, 11))   # cdir/ (圧縮) は拒否されるので manifest のまま
exp["saf_src"] = ("d", "0", "-"); exp["saf_dst"] = ("d", "0", "-"); put("saf_dst/move_me.bin", pat(70_000, 2))
got = {}
for dp, dns, fns in os.walk(root):
    for n in dns + fns:
        p = os.path.join(dp, n); r = os.path.relpath(p, root)
        if r.startswith("$") or r.startswith("System Volume Information"): continue
        if os.path.isdir(p): got[r] = ("d", "0", "-")
        else:
            b = open(p, "rb").read(); got[r] = ("f", str(len(b)), hashlib.sha256(b).hexdigest())
bad = [p for p in set(exp) | set(got) if exp.get(p) != got.get(p)]
for p in sorted(bad)[:10]: print("MISMATCH", p, exp.get(p), got.get(p))
print("android write result: %d entries, %d mismatches" % (len(got), len(bad)))
sys.exit(1 if bad else 0)
PY
