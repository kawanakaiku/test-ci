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
import java.security.MessageDigest

/**
 * 実機 USB メモリへの**書き込み**試験 (NTFS / exFAT / ext2 / ext3 / ext4 / Btrfs / F2FS のみ。Btrfs と F2FS は自前の書き込み実装で、実機では未確認の実験段階)。1 回の実行で 1 本の USB の、指定した 1 パーティションだけを扱う。
 * 通常の AVD の試験では skip (`-e usbmem.real true` と `-e usbmem.write true` の両方が無ければ assumeTrue)。実行は tools/run_android_real_usb_verify.sh --write。
 *
 * 手順 (パーティションごと):
 *  1. [pre]   読み取り専用で開き、MANIFEST.sha256 の全ファイルを照合 (4.5 GiB のファイルは SHA-256 を省く) + 自前の fsck (DiskCheck, 読み取り専用) で状態を記録。
 *  2. [CreateOnly] 書き込みモード CreateOnly で開く。専用フォルダ usbmem_cotest_<時刻> を作って書き、既存ファイルへの書き込み・削除・名前変更・縮小が拒否されることを確認。
 *             CreateOnly は削除できないので、このフォルダは次の段階まで残る (自分で作ったものだけ)。閉じて、読み取り専用で再オープン → 軽い照合 + fsck。
 *  3. [Full]  書き込みモード Full で開く。CreateOnly で作ったフォルダを消し、WriteSelfTest.run (専用フォルダの中だけで 作成・書き込み・上書き・縮小・名前変更・移動・削除 + 毎回読み戻し + 後片付け)、
 *             速度の計測 (32 MiB)。閉じて、読み取り専用で再オープン → 全ファイル照合 + fsck + 残骸が無いことの確認。
 * 失敗したら、そのパーティションではそこで止める (後片付けは自動で試み、残骸があれば読み取り専用で一覧するだけ。手では消さない)。
 *
 * 安全: 書き込めるのは [WRITABLE_FS] だけ。FAT/exFAT 以外の FS・needs_recovery の ext (復旧検証用の固定イメージ)・MBR の ext4 は、指定されても拒否する。
 * instrument 引数 (usbmem.*): real=true write=true devices=<VID:PID 1 つ> part=<パーティション番号 1 つ> stage=createonly|full|both (既定 both) preverify=true|false
 */
class RealUsbWriteTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx: Context = inst.targetContext
    private val args = RealUsb.Args(InstrumentationRegistry.getArguments())

    companion object {
        /** 書き込みを許す FS (パーティション表の fs 名)。FAT32 は書き込み未対応、XFS/squashfs/erofs/UDF/ISO は読み取り専用 FS。 */
        val WRITABLE_FS = setOf("ntfs", "exfat", "ext2", "ext3", "ext4", "btrfs", "f2fs")
        const val BIG_SKIP = "02_big/big_4p5GiB.bin"
    }

    private fun pattern(n: Int, seed: Int) = ByteArray(n) { (((it * 7) + seed) and 255).toByte() }

    private fun exists(v: NtfsVolume, path: String): Boolean =
        try { v.stat(path); true } catch (e: NativeException) { if (e.code == 2) false else throw e }

    private fun stepJson(name: String, ok: Boolean, ms: Long, detail: String = "") = JSONObject().put("name", name).put("ok", ok).put("ms", ms).put("detail", detail)

    @Test
    fun writeOnePartition() {
        assumeTrue("実機 USB 専用 (-e usbmem.real true)", args.real)
        assumeTrue("書き込み試験は明示 (-e usbmem.write true)", args.bool("write"))
        val usb = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        val devs = RealUsb.massStorageDevices(usb, args.list("devices"))
        assertTrue("-e usbmem.devices には VID:PID を 1 つだけ指定してください (対象 ${devs.size} 本)", args.list("devices").size == 1 && devs.size == 1)
        val partNo = args.str("part").toIntOrNull()
        assertTrue("-e usbmem.part にパーティション番号を 1 つ指定してください", partNo != null)
        assumeTrue("BuildConfig.WRITE_SUPPORTED=false のビルド (-PusbmemNtfsWrite=true が必要)", BuildConfig.WRITE_SUPPORTED)
        val dev = devs[0]
        assertTrue("USB の許可がありません", RealUsb.ensurePermission(ctx, usb, dev, args.str("perm", "0").toLongOrNull() ?: 0L))
        val stage = args.str("stage", "both")
        val tag = args.str("tag", "W_" + java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date()))
        val stamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date())
        val mgr = UsbSessionManager(UsbStorageSessionFactory(ctx, usb), writeSupported = true)
        val serial = runCatching { dev.serialNumber }.getOrNull()
        val info = RealUsb.info(dev, serial)

        val root = JSONObject().put("kind", "write").put("tag", tag).put("vidPid", RealUsb.vidPid(dev)).put("partition", partNo).put("stage", stage)
        val stages = JSONArray(); root.put("stages", stages)
        var pass = false
        try {
            // ---- パーティションの特定と許可判定 (読み取りだけ)
            val p0 = mgr.open(info).let { s ->
                try {
                    val pt = s.partitionTable()
                    root.put("tableKind", pt.kind)
                    val p = pt.partitions.firstOrNull { it.index == partNo && !it.extendedContainer && !it.protective } ?: error("パーティション #$partNo がありません")
                    if (pt.kind == "mbr" && p.fs == "ext4") error("MBR の ext4 は復旧検証用の固定イメージ (needs_recovery) なので書き込みません")
                    if (p.fs !in WRITABLE_FS) error("fs=${p.fs} は書き込み対象外です (対象: $WRITABLE_FS)")
                    p
                } finally { mgr.close(dev.deviceName) }
            }
            root.put("target", RealUsb.partitionJson(p0))
            RealUsb.log("[write ${RealUsb.vidPid(dev)}#$partNo] target ${p0.name} fs=${p0.fs} size=${p0.sizeBytes shr 20} MiB stage=$stage")

            // ---- 1. pre
            val pre = readOnlyPass("pre", mgr, info, p0, listOf(BIG_SKIP), refuseNeedsRecovery = true)
            stages.put(pre)
            if (!pre.optBoolean("pass")) { root.put("stopped", "pre の照合/検査が通らないので書き込みません"); return finish(root, tag, false) }

            // ---- 2. CreateOnly
            if (stage == "createonly" || stage == "both") {
                val w = writeStage("createonly", WriteMode.CreateOnly, mgr, info, p0, stamp)
                stages.put(w)
                if (!w.optBoolean("pass")) { root.put("stopped", "CreateOnly の書き込みで失敗"); return finish(root, tag, false) }
                val post = readOnlyPass("post-createonly", mgr, info, p0, listOf("02_big/"), refuseNeedsRecovery = false, expectFolder = "usbmem_cotest_$stamp")
                stages.put(post)
                if (!post.optBoolean("pass")) { root.put("stopped", "CreateOnly 後の再オープン/照合で失敗"); return finish(root, tag, false) }
            }
            // ---- 3. Full
            if (stage == "full" || stage == "both") {
                val w = writeStage("full", WriteMode.Full, mgr, info, p0, stamp)
                stages.put(w)
                if (!w.optBoolean("pass")) { root.put("stopped", "Full の書き込みで失敗"); return finish(root, tag, false) }
                val post = readOnlyPass("post-full", mgr, info, p0, listOf(BIG_SKIP), refuseNeedsRecovery = false, expectNoResidue = true)
                stages.put(post)
                if (!post.optBoolean("pass")) { root.put("stopped", "Full 後の再オープン/照合で失敗"); return finish(root, tag, false) }
            }
            pass = true
        } catch (e: Throwable) {
            root.put("error", "${e.javaClass.simpleName}: ${e.message}")
            RealUsb.warn("write test aborted: ${e.javaClass.simpleName}: ${e.message}")
        } finally { runCatching { mgr.closeAll() } }
        finish(root, tag, pass)
    }

    private fun finish(root: JSONObject, tag: String, pass: Boolean) {
        root.put("pass", pass)
        RealUsb.writeReport(ctx, tag, root)
        RealUsb.log("== write test ${if (pass) "PASS" else "FAIL"} tag=$tag stopped=${root.optString("stopped")} error=${root.optString("error")}")
        assertTrue("書き込み試験が通りませんでした: ${root.optString("stopped")} ${root.optString("error")}", pass)
    }

    // ------------------------------------------------------------------------------------------------ 読み取り専用の通過

    /** 読み取り専用で開く → fsck (DiskCheck) → RO マウント → 全照合 → 閉じる。ダーティ/needs_recovery が立っていれば不合格。 */
    private fun readOnlyPass(name: String, mgr: UsbSessionManager, info: UsbDeviceInfo, p: Partition, skipHash: List<String>, refuseNeedsRecovery: Boolean,
                             expectFolder: String? = null, expectNoResidue: Boolean = false): JSONObject {
        val tag = "[write ${info.vendorId.toString(16)}:${info.productId.toString(16)}#${p.index}:$name]"
        val j = JSONObject().put("name", name).put("mode", "ReadOnly")
        val problems = JSONArray()
        val t0 = System.nanoTime()
        mgr.setWriteMode(info.deviceName, WriteMode.ReadOnly)
        val s = mgr.open(info)
        try {
            val chk = fsck(s, p)
            j.put("fsck", chk)
            RealUsb.log("$tag fsck: ${chk}")
            if (chk.optBoolean("unclean")) problems.put("fsck: unclean (ダーティ/未クリーン)")
            if (chk.optBoolean("needsJournalRecovery")) problems.put("fsck: needs_recovery")
            if (chk.optInt("errors") > 0) problems.put("fsck: errors=${chk.optInt("errors")}")
            val (mv, mj) = RealUsb.mountPartition(s, p, "", "")
            j.put("mount", mj)
            if (mv == null) {
                problems.put("RO マウント失敗: errno=${mj.optInt("errno")} ${mj.optString("errName")} ${mj.optString("message")}")
            } else mv.use { m ->
                if (mj.optBoolean("noload")) problems.put("noload でしか開けない (ジャーナル未再生 = ダーティ)")
                val v = m.volume
                val mani = RealUsb.parseManifest(String(RealUsb.readWhole(v, "/MANIFEST.sha256"), Charsets.UTF_8))
                val r = RealUsb.verifyTree(v, mani, tag, skipHash)
                j.put("verify", r.toJson())
                if (!r.ok) problems.put("照合 NG: problems=${r.problemCount}")
                RealUsb.log("$tag verify ${if (r.ok) "OK" else "NG"} files=${r.files} hashed=${r.bytes shr 20} MiB at %.1f MB/s problems=${r.problemCount} extra=${r.extraCount}".format(if (r.hashMs > 0) r.bytes / 1e6 / (r.hashMs / 1000.0) else 0.0))
                for (x in r.problems.take(10)) RealUsb.warn("$tag PROBLEM $x")
                if (expectNoResidue) {
                    val residue = r.extra.filter { it.startsWith("usbmem_") }
                    if (residue.isNotEmpty()) problems.put("専用フォルダの残骸: $residue")
                    j.put("residue", JSONArray(residue))
                }
                if (expectFolder != null) {
                    val ok = exists(v, "/$expectFolder")
                    j.put("expectedFolderPresent", ok)
                    if (!ok) problems.put("CreateOnly で作ったフォルダがありません: $expectFolder")
                    else {
                        // 中身が書いた通りか
                        val f = RealUsb.readWhole(v, "/$expectFolder/f.bin")
                        if (!f.contentEquals(pattern(300_000, 5))) problems.put("CreateOnly で書いた f.bin が一致しません")
                    }
                }
                // 残骸の一覧 (読み取りだけ)。usbmem_ で始まるものは、期待するフォルダ以外は報告する
                val others = r.extra.filter { it.startsWith("usbmem_") && it != expectFolder && !it.startsWith("$expectFolder/") }
                if (others.isNotEmpty()) j.put("otherUsbmemEntries", JSONArray(others))
            }
        } catch (e: Throwable) {
            problems.put("例外: ${e.javaClass.simpleName}: ${e.message}")
        } finally {
            val c0 = System.nanoTime()
            runCatching { mgr.close(info.deviceName) }
            j.put("closeMs", (System.nanoTime() - c0) / 1_000_000)
        }
        j.put("problems", problems).put("pass", problems.length() == 0).put("ms", (System.nanoTime() - t0) / 1_000_000)
        return j
    }

    private fun fsck(s: UsbSession, p: Partition): JSONObject {
        val t0 = System.nanoTime()
        return try {
            val r = s.withHandle { h -> DiskCheck.check(h, p.startBytes, p.sizeBytes) }
            JSONObject().put("fs", r.fs).put("verdict", r.verdict).put("unclean", r.unclean).put("needsJournalRecovery", r.needsJournalRecovery)
                .put("errors", r.errors).put("warnings", r.warnings).put("infos", r.infos)
                .put("findings", JSONArray(r.findings.take(12).map { "${it.severity}:${it.code}:${it.message}" })).put("ms", (System.nanoTime() - t0) / 1_000_000)
        } catch (e: Throwable) {
            JSONObject().put("error", "${e.javaClass.simpleName}: ${e.message}").put("errors", 1)
        }
    }

    // ------------------------------------------------------------------------------------------------ 書き込みの段階

    private fun writeStage(name: String, mode: WriteMode, mgr: UsbSessionManager, info: UsbDeviceInfo, p: Partition, stamp: String): JSONObject {
        val tag = "[write ${info.vendorId.toString(16)}:${info.productId.toString(16)}#${p.index}:$name]"
        val j = JSONObject().put("name", name).put("mode", mode.name)
        val steps = JSONArray(); j.put("steps", steps)
        val t0 = System.nanoTime()
        assertTrue("setWriteMode", mgr.setWriteMode(info.deviceName, mode))
        val s = mgr.open(info)
        var allOk = true
        val coFolder = "usbmem_cotest_$stamp"
        val speedFolder = "usbmem_speed_$stamp"
        var cleanupFailed = false
        try {
            j.put("sessionWritable", s.writable)
            if (!s.writable) throw IllegalStateException("セッションが書き込み可で開かれていません")
            val (mv, mj) = RealUsb.mountPartition(s, p, "", "", writable = true, createOnly = mode != WriteMode.Full)
            j.put("mount", mj)
            if (mv == null) throw IllegalStateException("書き込みマウント失敗: errno=${mj.optInt("errno")} ${mj.optString("errName")} ${mj.optString("message")}")
            val v = mv.volume
            fun step(n: String, body: () -> String?): Boolean {
                val a = System.nanoTime()
                val r = try { body()?.let { false to it } ?: (true to "") } catch (e: Throwable) { false to "${e.javaClass.simpleName}: ${e.message}" }
                val ms = (System.nanoTime() - a) / 1_000_000
                steps.put(stepJson(n, r.first, ms, r.second))
                RealUsb.log("$tag step '$n' ${if (r.first) "ok" else "FAILED"} ${ms}ms ${r.second}")
                if (!r.first) allOk = false
                return r.first
            }
            try {
                if (!v.writable) throw IllegalStateException("ボリュームが読み取り専用でマウントされました (ダーティ等で書き込みを拒否)")
                if (v.createOnly != (mode != WriteMode.Full)) throw IllegalStateException("createOnly の状態が想定と違います")
                if (mode == WriteMode.CreateOnly) createOnlySteps(v, coFolder, ::step)
                else fullSteps(v, coFolder, speedFolder, stamp, steps, tag, ::step)
            } finally {
                // クリーンなアンマウント: sync → ボリュームを閉じる (書き込みの確定)。例外は記録する
                val c0 = System.nanoTime()
                val syncOk = try { v.sync(); true } catch (e: Throwable) { steps.put(stepJson("アンマウント前の sync", false, 0, "${e.javaClass.simpleName}: ${e.message}")); allOk = false; false }
                val closeOk = try { mv.close(); true } catch (e: Throwable) { steps.put(stepJson("ボリュームを閉じる", false, 0, "${e.javaClass.simpleName}: ${e.message}")); allOk = false; false }
                steps.put(stepJson("クリーンアンマウント (sync + close)", syncOk && closeOk, (System.nanoTime() - c0) / 1_000_000))
            }
        } catch (e: Throwable) {
            allOk = false
            steps.put(stepJson("準備", false, 0, "${e.javaClass.simpleName}: ${e.message}"))
            RealUsb.warn("$tag aborted: ${e.message}")
        } finally {
            val c0 = System.nanoTime()
            runCatching { mgr.close(info.deviceName) }
            j.put("sessionCloseMs", (System.nanoTime() - c0) / 1_000_000)
            runCatching { j.put("opStats", s.opStats()) }
        }
        j.put("pass", allOk && !cleanupFailed).put("ms", (System.nanoTime() - t0) / 1_000_000)
        return j
    }

    private fun writeBig(v: NtfsVolume, path: String, bytes: Long, seed: Int): Long {
        val chunk = pattern(1 shl 20, seed)
        var off = 0L
        val t0 = System.nanoTime()
        while (off < bytes) {
            val n = minOf(chunk.size.toLong(), bytes - off).toInt()
            val w = v.write(path, off, chunk, n)
            if (w <= 0) throw IllegalStateException("short write at $off")
            off += w
        }
        v.sync()
        return (System.nanoTime() - t0) / 1_000_000
    }

    private fun readBigCheck(v: NtfsVolume, path: String, bytes: Long, seed: Int): Pair<Boolean, Long> {
        val buf = ByteArray(1 shl 20)
        var off = 0L
        val t0 = System.nanoTime()
        while (off < bytes) {
            val n = v.read(path, off, buf)
            if (n <= 0) return false to 0
            for (i in 0 until n) if (buf[i] != ((((off + i).toInt() * 7) + seed) and 255).toByte()) return false to 0
            off += n
        }
        return (off == bytes) to (System.nanoTime() - t0) / 1_000_000
    }

    private fun speedSteps(v: NtfsVolume, folder: String, step: (String, () -> String?) -> Boolean, mbs: Int = 32) {
        val bytes = mbs.toLong() shl 20
        val path = "/$folder/speed.bin"
        step("速度計測: ${mbs} MiB を書く") {
            v.create("/$folder", "speed.bin", false)
            val ms = writeBig(v, path, bytes, 11)
            RealUsb.log("write %.1f MB/s".format(bytes / 1e6 / (ms / 1000.0)))   // step は null = 成功。速度はログにだけ出す
            null
        }
        step("速度計測: ${mbs} MiB を読み戻して照合") {
            val (ok, ms) = readBigCheck(v, path, bytes, 11)
            if (!ok) "mismatch" else null.also { RealUsb.log("read-back %.1f MB/s".format(bytes / 1e6 / (ms / 1000.0))) }
        }
    }

    private fun createOnlySteps(v: NtfsVolume, folder: String, step: (String, () -> String?) -> Boolean) {
        val a = pattern(300_000, 5)
        step("専用フォルダを作る ($folder)") { v.create("/", folder, true); if (v.stat("/$folder").isDir) null else "not a directory" }
        step("ファイルを作って書く (300 KB)") {
            v.create("/$folder", "f.bin", false); writeAll(v, "/$folder/f.bin", a); v.sync()
            if (RealUsb.readWhole(v, "/$folder/f.bin").contentEquals(a)) null else "read-back mismatch"
        }
        step("小さいファイルを 10 個作る") {
            v.create("/$folder", "sub", true)
            for (i in 0 until 10) { v.create("/$folder/sub", "s$i.txt", false); writeAll(v, "/$folder/sub/s$i.txt", pattern(100 + i * 37, i)) }
            v.sync()
            var bad: String? = null
            for (i in 0 until 10) if (!RealUsb.readWhole(v, "/$folder/sub/s$i.txt").contentEquals(pattern(100 + i * 37, i))) bad = "s$i.txt mismatch"
            bad
        }
        speedSteps(v, folder, step)
        // 既存ファイルへの変更は CreateOnly が拒否する (ネイティブに届く前に拒否されるので、ディスクには触れない)
        fun refused(label: String, body: () -> Unit) = step("既存ファイルへの$label が拒否される") {
            try { body(); "拒否されませんでした" } catch (e: NativeException) { if (e.code == 1) null else "errno ${e.code} (想定 EPERM=1)" }
        }
        refused("書き込み") { v.write("/README.txt", 0, ByteArray(16)) }
        refused("縮小 (truncate)") { v.truncate("/README.txt", 1) }
        refused("名前変更") { v.rename("/README.txt", "README2.txt") }
        refused("削除") { v.remove("/README.txt") }
    }

    private fun writeAll(v: NtfsVolume, path: String, data: ByteArray) {
        var done = 0
        while (done < data.size) {
            val chunk = data.copyOfRange(done, minOf(data.size, done + 65536))
            val n = v.write(path, done.toLong(), chunk, chunk.size)
            if (n <= 0) throw IllegalStateException("short write at $done")
            done += n
        }
    }

    private fun fullSteps(v: NtfsVolume, coFolder: String, speedFolder: String, stamp: String, steps: JSONArray, tag: String, step: (String, () -> String?) -> Boolean) {
        // CreateOnly の段階で自分が作ったフォルダがあれば消す (この試験が作ったもの。他は一切消さない)
        if (exists(v, "/$coFolder")) {
            step("CreateOnly で作ったフォルダを消す ($coFolder)") { v.removeTree("/$coFolder"); v.sync(); if (exists(v, "/$coFolder")) "まだ残っています" else null }
        }
        // 前回の実行が CreateOnly の段階で止まって残った、この試験の専用フォルダ (名前が usbmem_cotest_<日付>_<時刻> に完全一致するものだけ)。他は一切消さない
        val stale = Regex("usbmem_cotest_\\d{8}_\\d{6}")
        v.list("/").map { it.name }.filter { it != coFolder && stale.matches(it) }.forEach { old ->
            step("前回の実行が残した専用フォルダを消す ($old)") { v.removeTree("/$old"); v.sync(); if (exists(v, "/$old")) "まだ残っています" else null }
        }
        val r = WriteSelfTest.run(v, stamp) { RealUsb.log("$tag selftest: $it") }
        for (st in r.steps) {
            steps.put(stepJson("WriteSelfTest: ${st.name}", st.ok, st.ms, st.detail))
            RealUsb.log("$tag selftest '${st.name}' ok=${st.ok} ${st.ms}ms ${st.detail}")
        }
        if (!r.passed) steps.put(stepJson("WriteSelfTest 全体", false, 0, "failed=${r.failedStep?.name} cleaned=${r.cleaned} folder=${r.folder}"))
        // 速度計測用フォルダ (作る → 書く → 読む → 中身ごと消す)
        step("速度計測用フォルダを作る") { v.create("/", speedFolder, true); null }
        speedSteps(v, speedFolder, step)
        step("速度計測用フォルダを消す") { v.removeTree("/$speedFolder"); v.sync(); if (exists(v, "/$speedFolder")) "まだ残っています" else null }
        step("専用フォルダが何も残っていない") {
            val left = v.list("/").map { it.name }.filter { it.startsWith("usbmem_") }
            if (left.isEmpty()) null else "残骸: $left"
        }
        if (!r.passed) step("WriteSelfTest が通った") { "failed: ${r.failedStep?.name}" }
    }
}
