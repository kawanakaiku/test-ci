#!/usr/bin/env python3
"""AndroidManifest.xml の権限・機能・コンポーネントの宣言漏れを、ソースの API 使用と突き合わせて検査する。

背景: `BiometricManager.canAuthenticate` は USE_BIOMETRIC が無いと SecurityException を投げる。マニフェストに無いのに CI の計装テストは
偽の vault を差すので通ってしまい、実機・AVD でパスワードの記憶が一切出なかった (docs/crypto-volumes.md 末尾の「修正 (2026-11-19)」)。
同種の漏れを、ビルドもエミュレータも要らずに (数秒で) 検出する。

検査:
  1. 漏れ: android/app/src/main の Kotlin / Java が使う API (規則表 RULES) が要る権限 / uses-feature が、マニフェストに無ければ失敗。
  2. 余り: マニフェストの uses-permission が、どの規則・`Manifest.permission.X` / "android.permission.X" の使用でも根拠づけられなければ失敗
     (不要な権限は審査・利用者の不信のもと)。
  3. コンポーネント: Activity / Service / BroadcastReceiver / ContentProvider / DocumentsProvider を継承する名前つきクラスが、マニフェストに
     <activity> / <service> / <receiver> / <provider> として宣言されていなければ失敗。

使い方: tools/check_manifest_permissions.py [--manifest PATH] [--src DIR ...] [-q]    終了コード: 0 = 問題なし、1 = 問題あり、2 = 入力の誤り。
規則表に API を足すときは、`tools/test_check_manifest_permissions.py` にも 1 件足す。
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ANDROID = "{http://schemas.android.com/apk/res/android}"
P = "android.permission."
ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = ROOT / "android/app/src/main/AndroidManifest.xml"
DEFAULT_SRC = [ROOT / "android/app/src/main/java"]

# name: 規則名 / pattern: (コメントを除いた) ソースに対する正規表現 / any_of: このうちどれか 1 つが要る権限 /
# features: 要る uses-feature (name -> required="false" でなければならないなら False) / why: 失敗時の説明
RULES = [
    dict(name="biometric", pattern=r"\b(BiometricManager|BiometricPrompt)\b|androidx\.biometric",
         any_of=[P + "USE_BIOMETRIC"],
         why="BiometricManager.canAuthenticate / BiometricPrompt は USE_BIOMETRIC が無いと SecurityException (記憶の機能が一切出なくなる)"),
    dict(name="fingerprint", pattern=r"\bFingerprintManager(Compat)?\b", any_of=[P + "USE_FINGERPRINT", P + "USE_BIOMETRIC"],
         why="FingerprintManager は USE_FINGERPRINT (または USE_BIOMETRIC) が要る"),
    dict(name="usb-host", pattern=r"\bUsbManager\b|\bUsbDevice\b|\bUsbDeviceConnection\b", features={"android.hardware.usb.host": False},
         why="USB Host を使うので uses-feature android.hardware.usb.host を宣言する (SAF でイメージを開けるので required=\"false\")"),
    dict(name="foreground-service", pattern=r"\bstartForeground\w*\s*\(|\bForegroundService\w*\b|FOREGROUND_SERVICE_TYPE",
         any_of=[P + "FOREGROUND_SERVICE"], why="フォアグラウンドサービスは FOREGROUND_SERVICE (API 34 以降は種類ごとの FOREGROUND_SERVICE_<TYPE> も) が要る"),
    dict(name="notifications", pattern=r"\bNotificationManager(Compat)?\b|\bNotificationCompat\b|\bNotification\.Builder\b|\bNotificationChannel\b",
         any_of=[P + "POST_NOTIFICATIONS"], why="通知は API 33 以降 (targetSdk 33+) POST_NOTIFICATIONS が要る"),
    dict(name="internet", pattern=r"\bjava\.net\.(Socket|ServerSocket|DatagramSocket|URL|HttpURLConnection|InetSocketAddress)\b|\bHttpsURLConnection\b|\bokhttp3\b|\bHttpURLConnection\b|\bSocket\s*\(|\bURL\s*\(",
         any_of=[P + "INTERNET"], why="ネットワーク通信は INTERNET が要る"),
    dict(name="network-state", pattern=r"\bConnectivityManager\b|\bNetworkCapabilities\b", any_of=[P + "ACCESS_NETWORK_STATE"], why="ACCESS_NETWORK_STATE が要る"),
    dict(name="wifi-state", pattern=r"\bWifiManager\b", any_of=[P + "ACCESS_WIFI_STATE", P + "CHANGE_WIFI_STATE"], why="WifiManager は ACCESS_WIFI_STATE などが要る"),
    dict(name="external-storage",
         pattern=r"\bEnvironment\.(getExternalStorageDirectory|getExternalStoragePublicDirectory|isExternalStorageManager)\b|ACTION_MANAGE_(ALL_FILES|APP_ALL_FILES)_ACCESS_PERMISSION|\"/(sdcard|storage/emulated)\b",
         any_of=[P + "READ_EXTERNAL_STORAGE", P + "WRITE_EXTERNAL_STORAGE", P + "MANAGE_EXTERNAL_STORAGE"],
         why="共有ストレージへ直接アクセスする API は READ/WRITE_EXTERNAL_STORAGE か MANAGE_EXTERNAL_STORAGE が要る (このアプリは SAF だけで足りるはず)"),
    dict(name="media", pattern=r"\bMediaStore\.(Images|Video|Audio|Files)\b", any_of=[P + "READ_EXTERNAL_STORAGE", P + "READ_MEDIA_IMAGES", P + "READ_MEDIA_VIDEO", P + "READ_MEDIA_AUDIO"],
         why="MediaStore の問い合わせは READ_EXTERNAL_STORAGE / READ_MEDIA_* が要る"),
    dict(name="wake-lock", pattern=r"\bnewWakeLock\s*\(", any_of=[P + "WAKE_LOCK"], why="WakeLock は WAKE_LOCK が要る"),
    dict(name="vibrate", pattern=r"\bVibrator(Manager)?\b|\bVibrationEffect\b", any_of=[P + "VIBRATE"], why="VIBRATE が要る"),
    dict(name="camera", pattern=r"android\.hardware\.camera2|\bCameraManager\b|android\.hardware\.Camera\b", any_of=[P + "CAMERA"], why="CAMERA が要る"),
    dict(name="audio-record", pattern=r"\bAudioRecord\b|\bMediaRecorder\b", any_of=[P + "RECORD_AUDIO"], why="RECORD_AUDIO が要る"),
    dict(name="location", pattern=r"\bLocationManager\b|\bFusedLocationProviderClient\b", any_of=[P + "ACCESS_FINE_LOCATION", P + "ACCESS_COARSE_LOCATION"], why="位置情報の権限が要る"),
    dict(name="bluetooth", pattern=r"\bBluetoothAdapter\b|\bBluetoothManager\b|\bBluetoothDevice\b", any_of=[P + "BLUETOOTH_CONNECT", P + "BLUETOOTH", P + "BLUETOOTH_SCAN"], why="Bluetooth の権限が要る"),
    dict(name="boot", pattern=r"\bACTION_BOOT_COMPLETED\b", any_of=[P + "RECEIVE_BOOT_COMPLETED"], why="RECEIVE_BOOT_COMPLETED が要る"),
    dict(name="exact-alarm", pattern=r"\bsetExact(AndAllowWhileIdle)?\s*\(|\bsetAlarmClock\s*\(", any_of=[P + "SCHEDULE_EXACT_ALARM", P + "USE_EXACT_ALARM"], why="正確なアラームは SCHEDULE_EXACT_ALARM が要る"),
    dict(name="install-packages", pattern=r"\bACTION_INSTALL_PACKAGE\b|\bPackageInstaller\b", any_of=[P + "REQUEST_INSTALL_PACKAGES"], why="REQUEST_INSTALL_PACKAGES が要る"),
    dict(name="write-settings", pattern=r"\bSettings\.System\.put\w*\s*\(|\bSettings\.System\.canWrite\b", any_of=[P + "WRITE_SETTINGS"], why="WRITE_SETTINGS が要る"),
    dict(name="overlay", pattern=r"\bTYPE_APPLICATION_OVERLAY\b|\bACTION_MANAGE_OVERLAY_PERMISSION\b", any_of=[P + "SYSTEM_ALERT_WINDOW"], why="SYSTEM_ALERT_WINDOW が要る"),
]
# ソース上の `Manifest.permission.X` / "android.permission.X" は、その権限そのものの宣言が要る (ランタイム権限の要求など)。
# ただし他のアプリの権限を要求する属性 (プロバイダの android:permission="MANAGE_DOCUMENTS" など) はマニフェスト側なので、ソースの文字列だけを見る。
EXPLICIT = re.compile(r"\bManifest\.permission\.([A-Z0-9_]+)\b|\"android\.permission\.([A-Z0-9_]+)\"")

COMPONENT_BASES = {
    "activity": ["Activity", "ComponentActivity", "FragmentActivity", "AppCompatActivity"],
    "service": ["Service", "JobService", "IntentService", "LifecycleService"],
    "receiver": ["BroadcastReceiver"],
    "provider": ["ContentProvider", "DocumentsProvider"],
}
CLASS_DECL = re.compile(r"^[ \t]*(?:(?:public|internal|private|protected|abstract|open|final)[ \t]+)*class[ \t]+(\w+)\b[^{\n]*?:[ \t]*([\w.]+)[ \t]*\(", re.M)
JAVA_CLASS_DECL = re.compile(r"\bclass[ \t]+(\w+)[ \t]+extends[ \t]+([\w.]+)")


def strip_comments(src: str) -> str:
    """// と /* */ のコメントを (行数を保って) 空白にする。文字列 / 文字リテラルの中は残す。"""
    out, i, n = [], 0, len(src)
    while i < n:
        c, d = src[i], src[i:i + 2]
        if d == "//":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif d == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j])); i = j
        elif src.startswith('"""', i):
            j = src.find('"""', i + 3)
            j = n if j < 0 else j + 3
            out.append(src[i:j]); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        else:
            out.append(c); i += 1
    return "".join(out)


def load_manifest(path: Path):
    root = ET.parse(path).getroot()
    perms = {e.get(ANDROID + "name") for e in root.findall("uses-permission")}
    perms |= {e.get(ANDROID + "name") for e in root.findall("uses-permission-sdk-23")}
    feats = {e.get(ANDROID + "name"): e.get(ANDROID + "required", "true") for e in root.findall("uses-feature")}
    comps = {}
    app = root.find("application")
    if app is not None:
        for kind in COMPONENT_BASES:
            for e in app.findall(kind):
                comps.setdefault(kind, set()).add(e.get(ANDROID + "name") or "")
        if app.get(ANDROID + "name"):
            comps.setdefault("application", set()).add(app.get(ANDROID + "name"))
    return perms, feats, comps, root.get("package")


def scan(src_dirs):
    """[(path, text (コメント除去済み))]"""
    files = []
    for d in src_dirs:
        for p in sorted(Path(d).rglob("*")):
            if p.suffix in (".kt", ".java"):
                files.append((p, strip_comments(p.read_text(encoding="utf-8", errors="replace"))))
    return files


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def comp_declared(name: str, declared: set) -> bool:
    short = name.rsplit(".", 1)[-1]
    return any(d.rsplit(".", 1)[-1] == short for d in declared)


def check(manifest: Path, src_dirs, quiet=False):
    perms, feats, comps, _ = load_manifest(manifest)
    files = scan(src_dirs)
    errors, justified, lines = [], set(), []

    def rel(p):
        try:
            return str(p.relative_to(ROOT))
        except ValueError:
            return str(p)

    for r in RULES:
        rx = re.compile(r["pattern"])
        hits = [(rel(p), line_of(t, m.start()), m.group(0)) for p, t in files for m in rx.finditer(t)]
        if not hits:
            continue
        where = ", ".join(f"{f}:{ln}" for f, ln, _ in hits[:3]) + (f" ほか {len(hits) - 3} 件" if len(hits) > 3 else "")
        need_any = r.get("any_of")
        if need_any:
            justified |= set(need_any)
            if not perms & set(need_any):
                errors.append(f"[{r['name']}] 権限の宣言漏れ: {' または '.join(need_any)} が AndroidManifest に無い。{r['why']}。使用箇所: {where}")
                continue
        for feat, required_ok in (r.get("features") or {}).items():
            if feat not in feats:
                errors.append(f"[{r['name']}] uses-feature の宣言漏れ: {feat}。{r['why']}。使用箇所: {where}")
            elif not required_ok and feats[feat] != "false":
                errors.append(f"[{r['name']}] uses-feature {feat} が required=\"false\" でない。{r['why']}")
        lines.append(f"  ok  [{r['name']}] {len(hits)} 件 ({where})")

    explicit = {}
    for p, t in files:
        for m in EXPLICIT.finditer(t):
            name = P + (m.group(1) or m.group(2))
            explicit.setdefault(name, f"{rel(p)}:{line_of(t, m.start())}")
    for name, where in explicit.items():
        justified.add(name)
        if name not in perms:
            errors.append(f"[explicit] 権限の宣言漏れ: コードが {name} を名指ししているが AndroidManifest に無い ({where})")

    for name in sorted(perms - justified):
        errors.append(f"[unused] 余分な権限: {name} が AndroidManifest にあるが、規則表のどの API の使用からも必要と判断されない。"
                      f"本当に使うなら tools/check_manifest_permissions.py の RULES に使う API を足し、使わないなら消す")

    for p, t in files:
        decls = [(m.group(1), m.group(2)) for m in CLASS_DECL.finditer(t) if not re.search(r"\babstract\b", m.group(0).split("class", 1)[0])]   # abstract は宣言できない (共通部。具象クラスがマニフェストに載る) + [(m.group(1), m.group(2)) for m in JAVA_CLASS_DECL.finditer(t)]
        for cls, base in decls:
            base = base.rsplit(".", 1)[-1]
            for kind, bases in COMPONENT_BASES.items():
                if base in bases and not comp_declared(cls, comps.get(kind, set())):
                    errors.append(f"[component] <{kind}> の宣言漏れ: {cls} ({base} を継承、{rel(p)}) が AndroidManifest に無い")

    if not quiet:
        print(f"AndroidManifest: {rel(manifest)}")
        print(f"  宣言: uses-permission {sorted(x.replace(P, '') for x in perms)} / uses-feature {sorted(feats)} / "
              + " ".join(f"{k}={len(v)}" for k, v in sorted(comps.items())))
        print(f"  走査: {len(files)} ファイル、規則 {len(RULES)} 件")
        for ln in lines:
            print(ln)
    for e in errors:
        print("NG  " + e, file=sys.stderr)
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--src", type=Path, action="append", help="ソースのディレクトリ (複数可。既定: android/app/src/main/java)")
    ap.add_argument("-q", "--quiet", action="store_true")
    a = ap.parse_args(argv)
    src = a.src or DEFAULT_SRC
    for path in [a.manifest, *src]:
        if not Path(path).exists():
            print(f"入力が無い: {path}", file=sys.stderr)
            return 2
    errors = check(a.manifest, src, a.quiet)
    if errors:
        print(f"失敗: {len(errors)} 件", file=sys.stderr)
        return 1
    if not a.quiet:
        print("OK: 権限・機能・コンポーネントの宣言とソースの使用が一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
