package org.usbmem.app

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.util.concurrent.Executors
import java.util.concurrent.Future

/**
 * 実機に挿した USB メモリ (tools/make_real_usb_testset.py で作った検証セット) の全ファイルシステムを、**読み取り専用**でマウントして
 * 各ボリュームのルートの MANIFEST.sha256 と実際の全ファイル (サイズ + SHA-256 + リンク + 余分なファイル) を照合する。書き込みはしない。
 * 通常の AVD テストでは skip (`-e usbmem.real true` が無ければ assumeTrue)。実行は tools/run_android_real_usb_verify.sh。
 *
 * instrument 引数 (`-e usbmem.<名前> <値>`):
 *  real=true            必須 (実機 USB 専用)
 *  devices=0930:6544,.. 対象の VID:PID (16 進)。省略 = 接続中の MSC 全部
 *  mode=sequential|parallel  1 本ずつ開いて閉じる / 全部同時に開いて並列に読む (既定 sequential)
 *  only=1,2             パーティション番号の絞り込み (全デバイスに適用)
 *  skipHash=02_big/big_4p5GiB.bin,..  SHA-256 を省くパスの接頭辞 (サイズは確認する)
 *  luksPass / bitlockerPass  暗号ボリュームのパスワード (既定は docs/real-usb-testset.md のテスト用固定値。空文字なら解錠しない)
 *  perm=<秒>            USB の許可が無いとき、ダイアログを出して待つ秒数 (既定 0 = 待たない)
 *  tag=...              レポートファイル名 (files/real_usb_report_<tag>.json)。既定は時刻
 * 結果: logcat のタグ RealUsbVerify と、アプリの filesDir の JSON (run-as org.usbmem.app cat files/real_usb_report_<tag>.json)。
 */
class RealUsbVerifyTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx: Context = inst.targetContext
    private val args = RealUsb.Args(InstrumentationRegistry.getArguments())

    @Test
    fun verifyAllManifests() {
        assumeTrue("実機 USB 専用 (-e usbmem.real true)", args.real)
        val usb = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        val tag = args.str("tag", java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date()))
        val parallel = args.str("mode", "sequential") == "parallel"
        val devices = RealUsb.massStorageDevices(usb, args.list("devices"))
        RealUsb.log("== start tag=$tag mode=${if (parallel) "parallel" else "sequential"} devices=${devices.map { RealUsb.vidPid(it) + "@" + it.deviceName }}")
        assertTrue("対象の USB が接続されていません", devices.isNotEmpty())
        val permWait = args.str("perm", "0").toLongOrNull() ?: 0L
        for (d in devices) assertTrue("USB の許可がありません: ${RealUsb.vidPid(d)} (先に UI で許可するか、-e usbmem.perm 120)", RealUsb.ensurePermission(ctx, usb, d, permWait))

        val mgr = UsbSessionManager(UsbStorageSessionFactory(ctx, usb), writeSupported = BuildConfig.WRITE_SUPPORTED)
        val t0 = System.nanoTime()
        val results = ArrayList<JSONObject>()
        if (parallel) {
            // 先に全部開いてから(同時に開いている状態を作る)、デバイスごとのスレッドで並列に読む
            val sessions = HashMap<String, Pair<UsbSession?, JSONObject>>()
            for (d in devices) sessions[d.deviceName] = openOne(mgr, d)
            val pool = Executors.newFixedThreadPool(devices.size)
            try {
                val fs: List<Future<JSONObject>> = devices.map { d -> pool.submit<JSONObject> { readDevice(d, sessions.getValue(d.deviceName)) } }
                for (f in fs) results += f.get()
            } finally { pool.shutdown() }
            RealUsb.log("opened simultaneously: ${mgr.openDevices()}")
            for (d in devices) closeOne(mgr, d, results.first { it.getString("deviceName") == d.deviceName })
        } else {
            for (d in devices) {
                val opened = openOne(mgr, d)
                val j = readDevice(d, opened)
                closeOne(mgr, d, j)
                results += j
            }
        }
        val total = (System.nanoTime() - t0) / 1_000_000
        val root = JSONObject().put("kind", "verify").put("tag", tag).put("mode", if (parallel) "parallel" else "sequential").put("totalMs", total)
            .put("device", android.os.Build.MODEL).put("sdk", android.os.Build.VERSION.SDK_INT).put("args", JSONObject().put("devices", args.str("devices")).put("only", args.str("only")).put("skipHash", args.str("skipHash")))
            .put("devices", JSONArray(results))
        RealUsb.writeReport(ctx, tag, root)
        var failed = 0
        for (r in results) {
            val vols = r.optJSONArray("volumes") ?: JSONArray()
            if (r.optString("openError").isNotEmpty()) failed++
            for (i in 0 until vols.length()) { val v = vols.getJSONObject(i); if (!v.optBoolean("pass")) failed++ }
        }
        RealUsb.log("== done totalMs=$total failedVolumes=$failed")
        assertTrue("照合に失敗したボリュームがあります ($failed)。レポートを見てください", failed == 0)
    }

    private fun openOne(mgr: UsbSessionManager, d: UsbDevice): Pair<UsbSession?, JSONObject> {
        val j = JSONObject().put("deviceName", d.deviceName).put("vidPid", RealUsb.vidPid(d)).put("product", d.productName ?: "")
        runCatching { j.put("serial", d.serialNumber ?: "") }
        val t0 = System.nanoTime()
        return try {
            val s = mgr.open(RealUsb.info(d, j.optString("serial").ifEmpty { null }))
            j.put("openMs", (System.nanoTime() - t0) / 1_000_000)
            RealUsb.log("opened ${j.getString("vidPid")} ${d.deviceName} in ${j.getLong("openMs")} ms (tuning=${s.tuningIndex} writable=${s.writable})")
            s to j
        } catch (e: Throwable) {
            val code = (e as? NativeException)?.code
            j.put("openError", "${e.javaClass.simpleName}: ${e.message}${code?.let { " (errno $it ${RealUsb.errName(it)})" } ?: ""}")
            RealUsb.warn("open FAILED ${j.getString("vidPid")}: ${j.getString("openError")}")
            null to j
        }
    }

    private fun closeOne(mgr: UsbSessionManager, d: UsbDevice, j: JSONObject) {
        val t0 = System.nanoTime()
        val closed = mgr.close(d.deviceName)
        j.put("closeMs", (System.nanoTime() - t0) / 1_000_000).put("closed", closed)
    }

    private fun readDevice(d: UsbDevice, opened: Pair<UsbSession?, JSONObject>): JSONObject {
        val (s, j) = opened
        val tag = "[${j.getString("vidPid")}]"
        if (s == null) return j
        val t0 = System.nanoTime()
        try {
            val pt = s.partitionTable()
            j.put("table", JSONObject().put("kind", pt.kind).put("sectorSize", pt.sectorSize).put("diskBytes", pt.diskBytes).put("usedBackupGpt", pt.usedBackupGpt)
                .put("diagnostics", JSONArray(pt.diagnostics.map { "${it.severity}:${it.code}:${it.message}" })))
            val vols = JSONArray()
            val parts = RealUsb.targetPartitions(pt, args.list("only"))
            RealUsb.log("$tag partition table: ${pt.kind} ${pt.diskBytes shr 20} MiB, ${parts.size} partitions: " + parts.joinToString { "#${it.index}:${it.fs}" })
            for (p in parts) vols.put(readPartition(s, p, tag))
            j.put("volumes", vols)
        } catch (e: Throwable) {
            j.put("openError", "partition scan: ${e.javaClass.simpleName}: ${e.message}")
            RealUsb.warn("$tag partition scan failed: ${e.message}")
        }
        j.put("readMs", (System.nanoTime() - t0) / 1_000_000)
        return j
    }

    private fun readPartition(s: UsbSession, p: Partition, devTag: String): JSONObject {
        val tag = "$devTag#${p.index}"
        val j = RealUsb.partitionJson(p)
        val t0 = System.nanoTime()
        val (mv, mj) = RealUsb.mountPartition(s, p, args.str("luksPass", "usbmem-test-pass"), args.str("bitlockerPass", "synth-Password-1"))
        j.put("mount", mj)
        if (mv == null) {
            val bl = mj.optJSONObject("crypto")?.optString("format", "")?.contains("BitLocker", true) == true || p.name.contains("bitlocker", true)
            j.put("knownUnresolved", bl).put("pass", bl)
            RealUsb.warn("$tag (${p.name}, fs=${p.fs}) mount FAILED: errno=${mj.optInt("errno")} ${mj.optString("errName")} ${mj.optString("message")}${if (bl) " [既知の未解決: BitLocker 風]" else ""}")
            return j
        }
        mv.use { m ->
            val v = m.volume
            RealUsb.log("$tag (${p.name}) mounted fs=${v.fsName} noload=${mj.optBoolean("noload")} in ${mj.optLong("mountMs")} ms")
            val mani = try { RealUsb.parseManifest(String(RealUsb.readWhole(v, "/MANIFEST.sha256"), Charsets.UTF_8)) } catch (e: Throwable) {
                j.put("pass", false).put("manifestError", "${e.javaClass.simpleName}: ${e.message}")
                RealUsb.warn("$tag MANIFEST.sha256 を読めません: ${e.message}")
                return j
            }
            j.put("manifestEntries", mani.size)
            val r = RealUsb.verifyTree(v, mani, tag, args.list("skipHash"))
            j.put("verify", r.toJson()).put("pass", r.ok).put("verifyMs", (System.nanoTime() - t0) / 1_000_000)
            RealUsb.log("$tag SUMMARY fs=${v.fsName} ${if (r.ok) "OK" else "NG"} files=${r.files} dirs=${r.dirs} symlinks=${r.links}(+${r.linksUnsupported} unverifiable) hardlinks=${r.hardlinks} " +
                "hashed=${r.bytes shr 20} MiB at %.1f MB/s skippedHash=${r.skippedHash} problems=${r.problemCount} extra=${r.extraCount}".format(if (r.hashMs > 0) r.bytes / 1e6 / (r.hashMs / 1000.0) else 0.0))
            for (pr in r.problems.take(15)) RealUsb.warn("$tag PROBLEM $pr")
            for (x in r.extra.take(10)) RealUsb.log("$tag EXTRA $x")
        }
        return j
    }
}
