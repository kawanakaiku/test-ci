#!/usr/bin/env python3
"""tools/check_manifest_permissions.py の試験: 実際のマニフェストは通り、過去の漏れ (USE_BIOMETRIC なし) など各種の漏れは検出される。
実行: python3 -I tools/test_check_manifest_permissions.py"""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_manifest_permissions as cmp  # noqa: E402

HEAD = '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n'
REAL = cmp.DEFAULT_MANIFEST.read_text(encoding="utf-8")


def run(manifest_xml, sources):
    """一時ディレクトリにマニフェストとソース ({ファイル名: 内容}) を作って検査し、(終了コード, 標準エラー) を返す。"""
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "src").mkdir()
        (d / "AndroidManifest.xml").write_text(manifest_xml, encoding="utf-8")
        for name, body in sources.items():
            (d / "src" / name).write_text(body, encoding="utf-8")
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = cmp.main(["--manifest", str(d / "AndroidManifest.xml"), "--src", str(d / "src"), "-q"])
        return rc, err.getvalue()


def manifest(perms=(), features=(), app=""):
    s = HEAD
    s += "".join(f'<uses-permission android:name="android.permission.{p}"/>\n' for p in perms)
    s += "".join(f'<uses-feature android:name="{f}" android:required="false"/>\n' for f in features)
    return s + f"<application>{app}</application></manifest>\n"


class RealTree(unittest.TestCase):
    def test_real_manifest_passes(self):
        self.assertEqual(cmp.main(["-q"]), 0)

    def test_real_manifest_without_use_biometric_fails(self):
        """2026-11-19 の不具合の再現: USE_BIOMETRIC の行を消したマニフェストは、実際のソースに対して失敗する。"""
        self.assertIn("USE_BIOMETRIC", REAL)
        stripped = "".join(ln for ln in REAL.splitlines(True) if "USE_BIOMETRIC" not in ln or "<!--" in ln)
        self.assertNotIn('"android.permission.USE_BIOMETRIC"', stripped)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "AndroidManifest.xml"
            p.write_text(stripped, encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                rc = cmp.main(["--manifest", str(p), "-q"])
        self.assertEqual(rc, 1)
        self.assertIn("[biometric]", err.getvalue())


class Synthetic(unittest.TestCase):
    def test_biometric_missing(self):
        rc, err = run(manifest(), {"A.kt": "val b = ctx.getSystemService(BiometricManager::class.java)"})
        self.assertEqual(rc, 1); self.assertIn("[biometric]", err)

    def test_biometric_declared(self):
        rc, err = run(manifest(["USE_BIOMETRIC"]), {"A.kt": "val b = ctx.getSystemService(BiometricManager::class.java)"})
        self.assertEqual(rc, 0, err)

    def test_comment_mentions_are_ignored(self):
        rc, err = run(manifest(), {"A.kt": "// BiometricManager, INTERNET, startForeground(1)\n/* UsbManager */\nval x = 1"})
        self.assertEqual(rc, 0, err)

    def test_usb_feature_missing_or_required(self):
        src = {"A.kt": "import android.hardware.usb.UsbManager"}
        self.assertEqual(run(manifest(), src)[0], 1)
        self.assertEqual(run(manifest(features=["android.hardware.usb.host"]), src)[0], 0)
        required = HEAD + '<uses-feature android:name="android.hardware.usb.host"/><application/></manifest>'
        rc, err = run(required, src)
        self.assertEqual(rc, 1); self.assertIn('required="false"', err)

    def test_foreground_service_and_notifications(self):
        rc, err = run(manifest(), {"A.kt": "startForeground(1, n)"})
        self.assertEqual(rc, 1); self.assertIn("FOREGROUND_SERVICE", err)
        rc, err = run(manifest(), {"A.kt": "val m = getSystemService(NotificationManager::class.java)"})
        self.assertEqual(rc, 1); self.assertIn("POST_NOTIFICATIONS", err)
        self.assertEqual(run(manifest(["POST_NOTIFICATIONS"]), {"A.kt": "val m = getSystemService(NotificationManager::class.java)"})[0], 0)

    def test_internet_and_external_storage(self):
        rc, err = run(manifest(), {"A.java": 'class A { void f() throws Exception { new java.net.Socket("h", 1); } }'})
        self.assertEqual(rc, 1); self.assertIn("INTERNET", err)
        rc, err = run(manifest(), {"A.kt": "val d = Environment.getExternalStorageDirectory()"})
        self.assertEqual(rc, 1); self.assertIn("EXTERNAL_STORAGE", err)

    def test_local_socket_is_not_network(self):
        self.assertEqual(run(manifest(), {"A.kt": "val s = LocalSocket()"})[0], 0)

    def test_explicit_permission_use(self):
        src = {"A.kt": "checkSelfPermission(Manifest.permission.CAMERA)"}
        self.assertEqual(run(manifest(), src)[0], 1)
        self.assertEqual(run(manifest(["CAMERA"]), src)[0], 0)

    def test_unused_permission(self):
        rc, err = run(manifest(["USE_BIOMETRIC", "VIBRATE"]), {"A.kt": "val b = BiometricManager::class"})
        self.assertEqual(rc, 1); self.assertIn("[unused]", err); self.assertIn("VIBRATE", err)

    def test_undeclared_component(self):
        src = {"S.kt": "class Sync : Service() {\n}\n", "R.kt": "class Boot : BroadcastReceiver() {\n}\n", "P.kt": "class Docs : DocumentsProvider() {\n}\n"}
        rc, err = run(manifest(), src)
        self.assertEqual(rc, 1)
        for k in ("<service>", "<receiver>", "<provider>"):
            self.assertIn(k, err)
        ok = manifest(app='<service android:name=".Sync"/><receiver android:name=".Boot"/><provider android:name=".Docs"/>')
        self.assertEqual(run(ok, src)[0], 0)

    def test_anonymous_receiver_is_not_a_component(self):
        self.assertEqual(run(manifest(), {"A.kt": "val r = object : BroadcastReceiver() { }"})[0], 0)


if __name__ == "__main__":
    unittest.main()
