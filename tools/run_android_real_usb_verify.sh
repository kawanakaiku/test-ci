#!/bin/sh
# 実機に挿した USB メモリ (tools/make_real_usb_testset.py で作った検証セット) を、Android アプリのプロセス内で
# 読み取り専用にマウントし、各ボリュームのルートの MANIFEST.sha256 と全ファイルを照合する計装テスト (RealUsbVerifyTest) の実行スクリプト。
# --write を付けると、書き込み試験 (RealUsbWriteTest。NTFS/exFAT/ext2/3/4 の 1 パーティションだけ。CreateOnly → Full) を行う。
# docs/real-usb-testset.md の「Android 実機での自動検証」。
#
# adb のシリアルは必須 (--serial か環境変数 ANDROID_SERIAL)。明示したシリアルの端末にしか触れない (emulator-* でも実機でも同じ。省略時の既定は無い)。
# 前提: JAVA_HOME / ANDROID_HOME (ビルドする場合)、対象の USB を端末に接続して、アプリで USB の許可を済ませておく
#       (許可が無ければ --perm 120 でダイアログを出して待つ。画面の OK は手でタップする)。OS が自動マウントした USB は `adb shell sm unmount public:X,Y` で外すこと。
# 使い方:
#   tools/run_android_real_usb_verify.sh --serial localhost:5565 [--devices 0930:6544[,VID:PID...]] [--mode sequential|parallel]
#        [--only 1,2] [--skip-hash 02_big/big_4p5GiB.bin] [--tag NAME] [--perm 秒] [--no-build] [--no-install] [--out DIR]
#   tools/run_android_real_usb_verify.sh --serial ... --write --devices 0930:6544 --part 1 [--lun N] [--stage createonly|full|both]
set -eu
export PATH="$PATH:/sbin:/usr/sbin"
SERIAL=${ANDROID_SERIAL:-}
DEVICES=""; MODE=sequential; ONLY=""; SKIP=""; TAG=""; PERM=0; BUILD=1; INSTALL=1; OUT=""; WRITE=0; PART=""; LUN=0; STAGE=both; EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL=$2; shift 2;;
    --devices) DEVICES=$2; shift 2;;
    --mode) MODE=$2; shift 2;;
    --only) ONLY=$2; shift 2;;
    --skip-hash) SKIP=$2; shift 2;;
    --tag) TAG=$2; shift 2;;
    --perm) PERM=$2; shift 2;;
    --no-build) BUILD=0; shift;;
    --no-install) INSTALL=0; shift;;
    --out) OUT=$2; shift 2;;
    --write) WRITE=1; shift;;
    --part) PART=$2; shift 2;;
    --lun) LUN=$2; shift 2;;
    --stage) STAGE=$2; shift 2;;
    --extra) EXTRA="$EXTRA -e $2 $3"; shift 3;;   # 例: --extra usbmem.luksPass xxx
    -h|--help) sed -n '2,16p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[ -n "$SERIAL" ] || { echo "adb のシリアルが必要です: --serial <serial> (または ANDROID_SERIAL)。既定は無い" >&2; exit 2; }
command -v adb >/dev/null || { echo "skip: adb missing" >&2; exit 77; }
ADB="adb -s $SERIAL"
$ADB get-state >/dev/null 2>&1 || { echo "端末 $SERIAL に接続できません" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "$WRITE" = 1 ]; then
  [ -n "$PART" ] && [ -n "$DEVICES" ] || { echo "--write には --devices <VID:PID 1 つ> と --part <番号> が必要です" >&2; exit 2; }
fi
[ -n "$TAG" ] || TAG=$( [ "$WRITE" = 1 ] && echo "W_$(date +%Y%m%d_%H%M%S)" || echo "V_$(date +%Y%m%d_%H%M%S)" )
[ -n "$OUT" ] || OUT="$HERE/../real_usb_reports"
mkdir -p "$OUT"

cleanup() { (cd "$HERE/../android" && ./gradlew --stop >/dev/null 2>&1) || true; }
trap cleanup EXIT
APKD="$HERE/../android/app/build/outputs/apk"
if [ "$BUILD" = 1 ]; then
  (cd "$HERE/../android" && nice -n 10 ./gradlew --no-daemon --offline :app:assembleDebug :app:assembleDebugAndroidTest -PusbmemNtfsWrite=true -q)
fi
if [ "$INSTALL" = 1 ]; then
  $ADB install -r -t "$APKD/debug/app-debug.apk" >/dev/null
  $ADB install -r -t "$APKD/androidTest/debug/app-debug-androidTest.apk" >/dev/null
fi

if [ "$WRITE" = 1 ]; then CLASS=org.usbmem.app.RealUsbWriteTest; else CLASS=org.usbmem.app.RealUsbVerifyTest; fi
$ADB logcat -c || true
set -- -e class $CLASS -e usbmem.real true -e usbmem.tag "$TAG" -e usbmem.perm "$PERM" -e usbmem.mode "$MODE"
[ -z "$DEVICES" ] || set -- "$@" -e usbmem.devices "$DEVICES"
[ -z "$ONLY" ] || set -- "$@" -e usbmem.only "$ONLY"
[ -z "$SKIP" ] || set -- "$@" -e usbmem.skipHash "$SKIP"
[ "$WRITE" = 0 ] || set -- "$@" -e usbmem.write true -e usbmem.part "$PART" -e usbmem.lun "$LUN" -e usbmem.stage "$STAGE"
echo "== instrument: $CLASS tag=$TAG serial=$SERIAL"
START=$(date +%s)
set +e
# shellcheck disable=SC2086
$ADB shell am instrument -w "$@" $EXTRA org.usbmem.app.test/androidx.test.runner.AndroidJUnitRunner > "$OUT/$TAG.instrument.txt" 2>&1
RC=$?
set -e
END=$(date +%s)
$ADB logcat -d -s RealUsbVerify:V > "$OUT/$TAG.logcat.txt" 2>&1 || true
$ADB shell run-as org.usbmem.app cat "files/real_usb_report_$TAG.json" > "$OUT/$TAG.json" 2>/dev/null || echo "(レポートファイルを回収できませんでした)" >&2
tail -5 "$OUT/$TAG.instrument.txt"
echo "== elapsed $((END - START)) s, instrument exit $RC. reports: $OUT/$TAG.{json,logcat.txt,instrument.txt}"

# 結果の要約表
python3 -I - "$OUT/$TAG.json" <<'PY' || true
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except Exception as e:
    print("レポートを読めません:", e); sys.exit(0)
if r.get("kind") == "verify":
    print("mode=%s total=%.1f s" % (r["mode"], r["totalMs"] / 1000))
    for d in r["devices"]:
        print("[%s] %s open=%sms %s" % (d.get("vidPid"), d.get("product"), d.get("openMs"), d.get("openError", "")))
        for v in d.get("volumes", []):
            m = v.get("mount", {}); vf = v.get("verify")
            line = "  #%d %-14s %-9s mount=%s" % (v["index"], v["name"], m.get("fsName", v["fs"]), "OK" if m.get("ok") else "NG(%s %s)" % (m.get("errName"), m.get("message")))
            if vf:
                line += " %s files=%d dirs=%d links=%d(+%d?) hard=%d hashed=%.0fMiB %.1fMB/s problems=%d extra=%d" % (
                    "OK" if vf["ok"] else "NG", vf["files"], vf["dirs"], vf["symlinks"], vf["symlinksUnverifiedUnsupported"], vf["hardlinks"], vf["hashedBytes"] / 1048576, vf["mbps"], vf["problemCount"], vf["extraCount"])
            if v.get("knownUnresolved"): line += " [既知の未解決]"
            print(line)
else:
    print("write test: pass=%s stopped=%s error=%s" % (r.get("pass"), r.get("stopped", ""), r.get("error", "")))
    for s in r.get("stages", []):
        print(" stage %-16s mode=%-10s pass=%s %.1fs %s" % (s["name"], s["mode"], s.get("pass"), s.get("ms", 0) / 1000, "; ".join(s.get("problems", []))))
        for st in s.get("steps", []):
            print("    %-4s %-50s %6dms %s" % ("ok" if st["ok"] else "NG", st["name"], st["ms"], st.get("detail", "")))
PY
exit $RC
