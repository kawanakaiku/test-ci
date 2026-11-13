#!/bin/sh
# Android エミュレータでの FAT12/16/32 書き込みテスト (実験的): 書き込み有効ビルド + 計装テスト (FatWriteTest) → 結果イメージを adb pull → ホストの独立実装で検証。docs/fat-write.md
#   検証: (1) fsck.fat -n が clean (FAT1/FAT2 の一致・FSInfo・連鎖・重複名を含む) (2) クリーンアンマウントされた (ブートセクタのダーティビットなし・FAT16/32 は FAT[1] のクリーン bit あり)
#         (3) 同じ操作列を**ホストのディレクトリ** (7z で展開した元イメージ) に POSIX 操作として実行した結果と、結果イメージを 7z で展開したツリー全体が一致。
#   ダーティ印の付いたイメージ (ブートセクタ / FAT[1]) は読み取り専用に落ちてバイト単位で不変 (計装テスト側で確認)。
# 前提: JAVA_HOME / ANDROID_HOME、エミュレータ起動済み (ANDROID_SERIAL=emulator-5556 など。実機は使わない)、フィクスチャ /dev/shm/usbmem_fat_$USER (tools/gen_fat_fixture.py)、dosfstools / 7z。
# 終了時に Gradle デーモンを止める (メモリ節約)。使い方: ANDROID_SERIAL=emulator-5556 tools/run_android_fat_write_test.sh [フィクスチャ dir]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the emulator (e.g. emulator-5556)}"
case "$ANDROID_SERIAL" in emulator-*) ;; *) echo "refusing: ANDROID_SERIAL must be an emulator" >&2; exit 2;; esac
HERE=$(cd "$(dirname "$0")" && pwd); D=${1:-/dev/shm/usbmem_fat_${USER:-u}}
for c in fsck.fat 7z python3 adb; do command -v $c >/dev/null || { echo "skip: $c missing" >&2; exit 77; }; done
# AVD で使うのは小さめの 3 種 (FAT12 / FAT16 / FAT32)。name:クラスタ:大きいファイル:many の数
CASES="fat12_c4k:4096:1500000:150 fat16_c512:512:3000000:200 fat32_c4k:4096:3000000:300"
for c in $CASES; do n=${c%%:*}; [ -f "$D/$n.img" ] || { echo "skip: no fixture $D/$n.img" >&2; exit 77; }; done
W=$(mktemp -d "${USBMEM_TMP:-/dev/shm}"/usbmem_afw.XXXXXX)
cleanup() {
  rm -rf "$W"
  adb shell 'rm -f /data/local/tmp/fat_wr_* /data/local/tmp/fat_dirty_*' 2>/dev/null || true
  (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true
}
trap cleanup EXIT
: > "$W/list.txt"
for c in $CASES; do
  IFS=: read name cl big nmany <<EOF
$c
EOF
  python3 -I "$HERE/gen_fat_ops.py" $cl "$W/ops_$name.txt" $nmany $big
  adb push "$D/$name.img" /data/local/tmp/fat_wr_$name.img >/dev/null
  adb push "$W/ops_$name.txt" /data/local/tmp/fat_wr_${name}_ops.txt >/dev/null
  echo "$name $big" >> "$W/list.txt"
done
adb push "$W/list.txt" /data/local/tmp/fat_wr_list.txt >/dev/null
# ダーティ印の付いたイメージ: ブートセクタの 0x25 (FAT12/16) の bit0 / FAT[1] のクリーン bit (FAT32: bit27) を落とす
cp "$D/fat16_c512.img" "$W/dirty_boot.img"
cp "$D/fat32_c4k.img" "$W/dirty_fat.img"
python3 -I - "$W/dirty_boot.img" "$W/dirty_fat.img" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'r+b'); f.seek(0x25); b = f.read(1)[0]; f.seek(0x25); f.write(bytes([b | 1])); f.close()
f = open(sys.argv[2], 'r+b'); bs = f.read(512)
bps = struct.unpack_from('<H', bs, 11)[0]; rsv = struct.unpack_from('<H', bs, 14)[0]
f.seek(rsv * bps + 4); v = struct.unpack('<I', f.read(4))[0]; f.seek(rsv * bps + 4); f.write(struct.pack('<I', v & ~(1 << 27))); f.close()
EOF
adb push "$W/dirty_boot.img" /data/local/tmp/fat_dirty_boot.img >/dev/null
adb push "$W/dirty_fat.img" /data/local/tmp/fat_dirty_fat.img >/dev/null
# USBMEM_SKIP_BUILD=1: ビルド済みの APK を使う (CI: tools/ci_android_tests.sh)
[ "${USBMEM_SKIP_BUILD:-0}" = 1 ] || (cd "$HERE/../android" && ./gradlew --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
# gradle の connected タスクはテスト後にアンインストールして結果イメージを消すため、install + am instrument を直接使う
APKD="$HERE/../android/app/build/outputs/apk"
adb install -r -t "$APKD/debug/app-debug.apk" >/dev/null
adb install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
for c in $CASES; do adb shell run-as org.usbmem.app rm -f cache/fat_rw_${c%%:*}.img 2>/dev/null || true; done
OUT=$(adb shell am instrument -w -e class org.usbmem.app.FatWriteTest org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner)
echo "$OUT" | tail -4
echo "$OUT" | grep -q "^OK (" || { echo "$OUT" | head -40; echo "FAIL: instrumentation"; exit 1; }
export LC_ALL=C.UTF-8 TZ=UTC
tree7z() {  # tree7z <img> <out>
  rm -rf "$W/x7"; mkdir "$W/x7"; 7z x -y -o"$W/x7" "$1" >/dev/null 2>&1 || return 1
  python3 -I "$HERE/ops_ref.py" --tree "$W/x7" > "$2"
}
rc=0
for c in $CASES; do
  IFS=: read name cl big nmany <<EOF
$c
EOF
  adb exec-out run-as org.usbmem.app cat cache/fat_rw_$name.img > "$W/r_$name.img"
  adb shell run-as org.usbmem.app rm -f cache/fat_rw_$name.img
  [ -s "$W/r_$name.img" ] || { echo "FAIL: result image missing ($name)"; rc=1; continue; }
  fsck.fat -n "$W/r_$name.img" >"$W/fsck.log" 2>&1 || { head -20 "$W/fsck.log"; echo "FAIL: fsck.fat not clean ($name)"; rc=1; continue; }
  # ダーティ印: ブートセクタ (FAT12/16: 0x25 / FAT32: 0x41) の bit0、FAT16 は FAT[1] bit15、FAT32 は bit27
  bad=$(python3 -I - "$W/r_$name.img" <<'EOF'
import struct, sys
f = open(sys.argv[1], 'rb'); bs = f.read(512)
bps = struct.unpack_from('<H', bs, 11)[0]; rsv = struct.unpack_from('<H', bs, 14)[0]; spf16 = struct.unpack_from('<H', bs, 22)[0]
fat32 = spf16 == 0
if bs[0x41 if fat32 else 0x25] & 1: print("boot sector dirty bit set"); sys.exit(0)
f.seek(rsv * bps + 4 if fat32 else rsv * bps + 2); v = f.read(4 if fat32 else 2)
x = struct.unpack('<I' if fat32 else '<H', v)[0]
nclus = None
if fat32 and not (x & (1 << 27)): print("FAT[1] clean bit missing")
elif (not fat32) and bs[0x26] == 0x29 and spf16 and not (x & 0x8000):
    # FAT12 には FAT[1] のクリーン bit が無い (ここでは FAT16 だけを見る: 12 ビット判定は mkfs.fat のクラスタ数による)
    tot = struct.unpack_from('<H', bs, 19)[0] or struct.unpack_from('<I', bs, 32)[0]
    spc = bs[13]; nroot = struct.unpack_from('<H', bs, 17)[0]
    data = tot - rsv - bs[16] * spf16 - (nroot * 32 + bps - 1) // bps
    if data // spc >= 4085: print("FAT[1] clean bit missing")
EOF
)
  [ -z "$bad" ] || { echo "FAIL: $name: $bad"; rc=1; continue; }
  # 独立実装 (ホストの POSIX) で同じ操作列を実行して比較
  tree7z "$D/$name.img" "$W/t0" || { echo "FAIL: 7z extract of the original ($name)"; rc=1; continue; }
  rm -rf "$W/ref"; cp -r "$W/x7" "$W/ref"
  python3 -I "$HERE/ops_ref.py" "$W/ref" "$W/ops_$name.txt" || { echo "FAIL: reference ops ($name)"; rc=1; continue; }
  python3 -I "$HERE/ops_ref.py" --tree "$W/ref" > "$W/tr"
  tree7z "$W/r_$name.img" "$W/ta" || { echo "FAIL: 7z extract of the result ($name)"; rc=1; continue; }
  if cmp -s "$W/ta" "$W/tr"; then echo "android fat write [$name]: $(wc -l < "$W/ta") entries identical to the host-reference tree, fsck.fat clean, dirty marks cleared"
  else diff "$W/ta" "$W/tr" | head -10; echo "FAIL: tree differs ($name)"; rc=1; fi
done
exit $rc
