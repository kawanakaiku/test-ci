package org.usbmem.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * 実機の USB メモリを使う計装テスト (RealUsbVerifyTest / RealUsbWriteTest) の共通部分。
 * 通常の AVD の試験では使われない (どちらのテストも `-e usbmem.real true` が無ければ assumeTrue で skip)。
 * 使い方: docs/real-usb-testset.md の「Android 実機での自動検証」、tools/run_android_real_usb_verify.sh。
 */
object RealUsb {
    const val TAG = "RealUsbVerify"

    /** instrument 引数。`-e usbmem.xxx 値` */
    class Args(private val m: android.os.Bundle) {
        fun str(k: String, def: String = ""): String = m.getString("usbmem.$k") ?: def
        fun bool(k: String, def: Boolean = false): Boolean = m.getString("usbmem.$k")?.let { it == "true" || it == "1" } ?: def
        fun list(k: String): List<String> = str(k).split(',').map { it.trim() }.filter { it.isNotEmpty() }
        val real: Boolean get() = bool("real")
    }

    fun log(msg: String) { Log.i(TAG, msg) }
    fun warn(msg: String) { Log.w(TAG, msg) }

    fun vidPid(d: UsbDevice) = "%04x:%04x".format(d.vendorId, d.productId)

    /** MSC (Bulk-Only) を持つ接続中のデバイス。[filter] = "0930:6544" の一覧 (空なら全部)。デバイス名順。 */
    fun massStorageDevices(usb: UsbManager, filter: List<String>): List<UsbDevice> =
        usb.deviceList.values.filter { UsbMscSelector.pick(UsbMscSelector.describe(it)) != null }
            .filter { filter.isEmpty() || vidPid(it) in filter.map { f -> f.lowercase() } }
            .sortedBy { it.deviceName }

    fun info(d: UsbDevice, serial: String?): UsbDeviceInfo = UsbDeviceInfo(d.deviceName, d.productName ?: "", d.vendorId, d.productId, serial)

    /** 許可が無ければダイアログを出して待つ (画面の「OK」は別途タップする)。許可されたら true。 */
    fun ensurePermission(ctx: Context, usb: UsbManager, d: UsbDevice, waitSec: Long): Boolean {
        if (usb.hasPermission(d)) return true
        if (waitSec <= 0) return false
        val latch = CountDownLatch(1)
        val rcv = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) { latch.countDown() }
        }
        val f = IntentFilter(UsbStorageSession.ACTION_PERMISSION)
        if (android.os.Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(rcv, f, Context.RECEIVER_NOT_EXPORTED) else @Suppress("UnspecifiedRegisterReceiverFlag") ctx.registerReceiver(rcv, f)
        try {
            usb.requestPermission(d, UsbStorageSession.permissionIntent(ctx))
            log("permission requested for ${vidPid(d)} ${d.deviceName}: waiting for the dialog (up to ${waitSec}s)")
            latch.await(waitSec, TimeUnit.SECONDS)
        } finally { runCatching { ctx.unregisterReceiver(rcv) } }
        return usb.hasPermission(d)
    }

    fun errName(code: Int): String = runCatching { NativeBridge.nativeErrcName(code) }.getOrDefault("?")

    fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

    // ---------------------------------------------------------------- マニフェスト

    /** kind = F / D / L / H。F の extra は "sparse" または空、L は リンク先、H は同じ inode のパス。 */
    data class Entry(val kind: Char, val path: String, val size: Long, val sha: String, val extra: String)

    /** tools/make_real_usb_testset.py の parse_manifest と同じ仕様 (タブ区切り、# はコメント)。 */
    fun parseManifest(text: String): List<Entry> {
        val out = ArrayList<Entry>()
        for (ln in text.split('\n')) {
            val line = ln.trimEnd('\r')
            if (line.isEmpty() || line.startsWith("#")) continue
            val c = line.split('\t')
            when (c[0]) {
                "D" -> out += Entry('D', c[3], 0, "-", "")
                "L" -> out += Entry('L', c[3], 0, "-", c.getOrElse(4) { "" })
                "H" -> out += Entry('H', c[3], c[1].toLong(), c[2], c.getOrElse(4) { "" })
                "F" -> out += Entry('F', c[3], c[1].toLong(), c[2], c.getOrElse(4) { "" })
            }
        }
        return out
    }

    /** verify_tree の IGNORED_EXTRA と同じ (ルート直下だけ)。rr_moved は ISO9660 の Rock Ridge で 8 階層超を退避する作成ツールのディレクトリ (カーネルの isofs もそのまま見せる)。 */
    val IGNORED_EXTRA = setOf("MANIFEST.sha256", "lost+found", "System Volume Information", "\$RECYCLE.BIN", ".Trash-0", ".Trash-1000", "found.000", ".fseventsd", ".Spotlight-V100", "rr_moved")

    fun readWhole(v: NtfsVolume, path: String, limit: Int = 8 shl 20): ByteArray {
        val size = v.stat(path).size
        require(size <= limit) { "$path is too large ($size)" }
        val out = ByteArray(size.toInt())
        var off = 0
        while (off < out.size) {
            val tmp = ByteArray(minOf(1 shl 20, out.size - off))
            val n = v.read(path, off.toLong(), tmp)
            if (n <= 0) break
            System.arraycopy(tmp, 0, out, off, n); off += n
        }
        return if (off == out.size) out else out.copyOf(off)
    }

    class TreeResult {
        var files = 0; var dirs = 0; var links = 0; var hardlinks = 0
        var linksUnsupported = 0       // readlink が未対応 (errno 95 / 38) で確認できなかった symlink
        var bytes = 0L                  // SHA-256 まで確認したバイト数
        var hashMs = 0L
        var skippedHash = 0             // --skip で SHA-256 を省いた (サイズは確認した) ファイル
        val problems = ArrayList<String>()
        var problemCount = 0
        val extra = ArrayList<String>()
        var extraCount = 0
        val ignoredExtra = ArrayList<String>()
        var aborted = false
        fun problem(s: String) { problemCount++; if (problems.size < 60) problems += s }
        val ok: Boolean get() = problemCount == 0 && !aborted
        fun toJson() = JSONObject().apply {
            put("ok", ok); put("files", files); put("dirs", dirs); put("symlinks", links); put("hardlinks", hardlinks)
            put("symlinksUnverifiedUnsupported", linksUnsupported); put("hashedBytes", bytes); put("hashMs", hashMs)
            put("mbps", if (hashMs > 0) bytes / 1e6 / (hashMs / 1000.0) else 0.0); put("skippedHash", skippedHash)
            put("problemCount", problemCount); put("problems", JSONArray(problems)); put("aborted", aborted)
            put("extraCount", extraCount); put("extra", JSONArray(extra)); put("ignoredExtra", JSONArray(ignoredExtra))
        }
    }

    /**
     * マニフェスト [entries] とボリュームの全ファイルを照合する (サイズ + SHA-256、symlink のリンク先、ハードリンクの同一 inode、余分なファイルの列挙)。
     * 読み取りだけ。[skipHash] = SHA-256 を省く (サイズは見る) パスの接頭辞。[progressMiB] = 大きいファイルの途中経過を出す間隔。
     */
    fun verifyTree(v: NtfsVolume, entries: List<Entry>, tag: String, skipHash: List<String> = emptyList(), progressMiB: Long = 512, maxProblems: Int = 400): TreeResult {
        val r = TreeResult()
        val known = HashSet<String>()
        for (e in entries) {
            known += e.path
            var p = e.path
            while (p.contains('/')) { p = p.substringBeforeLast('/'); known += p }   // 祖先のディレクトリも既知
        }
        val buf = ByteArray(1 shl 20)
        for (e in entries) {
            if (r.problemCount >= maxProblems) { r.aborted = true; warn("$tag: too many problems, abort"); break }
            val path = "/" + e.path
            try {
                when (e.kind) {
                    'D' -> { val st = v.stat(path); if (!st.isDir) r.problem("ディレクトリでない: ${e.path}") else r.dirs++ }
                    'F', 'H' -> {
                        val st = v.stat(path)
                        if (st.isDir || st.isLink) { r.problem("通常ファイルでない: ${e.path}"); continue }
                        if (st.size != e.size) { r.problem("サイズ不一致: ${e.path} (期待 ${e.size} / 実際 ${st.size})"); continue }
                        if (skipHash.any { e.path.startsWith(it) }) { r.skippedHash++; r.files++; continue }
                        val md = MessageDigest.getInstance("SHA-256")
                        val t0 = System.nanoTime()
                        var off = 0L; var nextLog = progressMiB shl 20
                        while (off < e.size) {
                            val n = v.read(path, off, buf)
                            if (n <= 0) break
                            md.update(buf, 0, n); off += n
                            if (e.size >= (256L shl 20) && off >= nextLog) {
                                val sec = (System.nanoTime() - t0) / 1e9
                                log("$tag: ${e.path} ${off shr 20}/${e.size shr 20} MiB (%.1f MB/s)".format(off / 1e6 / sec)); nextLog += progressMiB shl 20
                            }
                        }
                        r.hashMs += (System.nanoTime() - t0) / 1_000_000
                        if (off != e.size) { r.problem("短い読み取り: ${e.path} (期待 ${e.size} / 読めた $off)"); continue }
                        if (hex(md.digest()) != e.sha) { r.problem("sha256 不一致: ${e.path}"); continue }
                        r.files++; r.bytes += e.size
                        if (e.size >= (256L shl 20)) log("$tag: ${e.path} OK (${e.size shr 20} MiB)")
                        if (e.kind == 'H') {
                            r.hardlinks++
                            val other = v.stat("/" + e.extra)
                            if (other.ino != st.ino) r.problem("ハードリンクの inode が別: ${e.path} (${st.ino}) と ${e.extra} (${other.ino})")
                        }
                    }
                    'L' -> {
                        val st = v.stat(path)
                        if (!st.isLink) { r.problem("シンボリックリンクでない: ${e.path}"); continue }
                        val target = try { v.readlink(path) } catch (x: NativeException) {
                            if (x.code == 95 || x.code == 38) { r.linksUnsupported++; continue }   // この FS の readlink は未対応
                            throw x
                        }
                        if (target != e.extra) r.problem("リンク先不一致: ${e.path} (期待 ${e.extra} / 実際 $target)") else r.links++
                    }
                }
            } catch (x: NativeException) {
                r.problem("${if (x.code == 2) "欠落" else "読み取りエラー"}: ${e.path} (errno ${x.code} ${errName(x.code)})")
            } catch (x: Throwable) {
                r.problem("例外: ${e.path} (${x.javaClass.simpleName}: ${x.message})")
            }
        }
        // 余分なファイル (OS の自動マウントなどで増えたものの見分け用。失敗にはしない)
        try {
            val stack = ArrayDeque<String>(); stack.addLast("")
            var dirsSeen = 0
            while (stack.isNotEmpty() && dirsSeen < 20000) {
                val d = stack.removeLast(); dirsSeen++
                val list = try { v.list(if (d.isEmpty()) "/" else "/$d") } catch (x: NativeException) { r.problem("一覧エラー: /$d (errno ${x.code})"); continue }
                for (en in list) {
                    val rel = if (d.isEmpty()) en.name else "$d/${en.name}"
                    if (d.isEmpty() && en.name in IGNORED_EXTRA) { r.ignoredExtra += rel; continue }
                    if (rel !in known) { r.extraCount++; if (r.extra.size < 80) r.extra += rel }
                    if (en.isDir && !en.isLink) stack.addLast(rel)
                }
            }
        } catch (x: Throwable) { r.problem("余分ファイルの列挙で例外: ${x.javaClass.simpleName}: ${x.message}") }
        return r
    }

    // ---------------------------------------------------------------- パーティションのマウント (FS 自動判定、読み取り専用)

    class MountedVolume(val volume: NtfsVolume, private val closers: List<() -> Unit>, val json: JSONObject) : AutoCloseable {
        override fun close() { volume.close(); closers.forEach { runCatching { it() } } }
    }

    class MountFailure(val json: JSONObject)

    /**
     * パーティション [p] を読み取り専用でマウントする。needs_recovery (EUCLEAN 117) は allowNoload=true で再試行。
     * 失敗したら LUKS / BitLocker かを CryptoVolume.probe で調べ、[luksPass] / [bitlockerPass] で解錠してから中の FS をマウントする。
     */
    fun mountPartition(sess: UsbSession, p: Partition, luksPass: String, bitlockerPass: String, writable: Boolean = false, createOnly: Boolean = true): Pair<MountedVolume?, JSONObject> {
        val j = JSONObject()
        val t0 = System.nanoTime()
        fun ms() = (System.nanoTime() - t0) / 1_000_000
        try {
            val v = sess.mountNtfs(p.startBytes, p.sizeBytes, writable, createOnly, false)
            j.put("ok", true).put("fsName", v.fsName).put("noload", false).put("writable", v.writable).put("mountMs", ms())
            return MountedVolume(v, emptyList(), j) to j
        } catch (e: NativeException) {
            j.put("firstErrno", e.code).put("firstErrName", errName(e.code)).put("firstMessage", e.message)
            if (writable) { j.put("ok", false).put("errno", e.code).put("errName", errName(e.code)).put("message", e.message).put("mountMs", ms()); return null to j }
            if (e.code == 117 && !writable) {
                try {
                    val v = sess.mountNtfs(p.startBytes, p.sizeBytes, false, true, true)
                    j.put("ok", true).put("fsName", v.fsName).put("noload", true).put("journalIgnored", v.journalIgnored).put("writable", false).put("mountMs", ms())
                    return MountedVolume(v, emptyList(), j) to j
                } catch (e2: NativeException) {
                    j.put("ok", false).put("errno", e2.code).put("errName", errName(e2.code)).put("message", e2.message).put("mountMs", ms())
                    return null to j
                }
            }
            // 暗号ボリュームか
            var sub = 0L
            try {
                sub = sess.withHandle { h -> NativeBridge.nativeDeviceSubRange(h, p.startBytes, p.sizeBytes) }
                if (!Handle.valid(sub)) { j.put("ok", false).put("errno", e.code).put("errName", errName(e.code)).put("message", e.message).put("mountMs", ms()); return null to j }
                val info = try { CryptoVolume.probe(sub) } catch (x: CryptoException) { j.put("cryptoProbeError", "${x.code} ${errName(x.code)}"); null }
                if (info == null) {
                    NativeBridge.nativeClose(sub)
                    // 一過性の失敗か確かめるため、読み取り専用なら 1 度だけ待って再試行する (結果は retry* に記録)。再試行で開けたら成功として返す
                    if (!writable) {
                        Thread.sleep(300)
                        try {
                            val v = sess.mountNtfs(p.startBytes, p.sizeBytes, false, true, false)
                            warn("mount of partition #${p.index} failed once (errno ${e.code}) but succeeded on retry")
                            j.put("ok", true).put("fsName", v.fsName).put("noload", false).put("writable", false).put("mountMs", ms())
                                .put("retriedAfterErrno", e.code).put("retriedAfterErrName", errName(e.code))
                            return MountedVolume(v, emptyList(), j) to j
                        } catch (e3: NativeException) { j.put("retryErrno", e3.code) }
                    }
                    j.put("ok", false).put("errno", e.code).put("errName", errName(e.code)).put("message", e.message).put("mountMs", ms())
                    return null to j
                }
                j.put("crypto", JSONObject().put("format", info.format).put("cipher", info.cipher).put("supported", info.supported).put("note", info.note))
                val pw = when (info.kind) { CryptoKind.Luks -> luksPass; CryptoKind.BitLocker -> bitlockerPass; else -> "" }
                if (pw.isEmpty()) {
                    NativeBridge.nativeClose(sub)
                    j.put("ok", false).put("errno", e.code).put("errName", errName(e.code)).put("message", "暗号ボリューム (${info.format}) だがパスワード未指定").put("mountMs", ms())
                    return null to j
                }
                val dec = try { CryptoVolume.open(sub, info.kind, password = pw.toByteArray(Charsets.UTF_8)) } catch (x: CryptoException) {
                    NativeBridge.nativeClose(sub)
                    j.put("ok", false).put("errno", x.code).put("errName", errName(x.code)).put("message", "解錠失敗 (${info.format}): ${x.message}${x.info?.note?.let { " / $it" } ?: ""}").put("unlockFailed", true).put("mountMs", ms())
                    return null to j
                }
                try {
                    val err = IntArray(1)
                    val pt = NativeBridge.nativeListPartitions(dec, err)?.let { PartitionTable.parse(String(it, Charsets.UTF_8)) }
                    val len = pt?.partitions?.firstOrNull()?.sizeBytes?.takeIf { it > 0 } ?: pt?.diskBytes ?: 0L
                    if (len <= 0) throw NativeException(err[0], "復号デバイスのサイズを取れません")
                    var noload = false
                    val v = try { NtfsVolume.mountDeviceRange(dec, 0, len, false) } catch (x: NativeException) {
                        if (x.code == 117) { noload = true; NtfsVolume.mountDeviceRange(dec, 0, len, true) } else throw x
                    }
                    j.put("ok", true).put("fsName", v.fsName).put("noload", noload).put("writable", false).put("unlocked", info.format).put("mountMs", ms())
                    val closers = listOf({ NativeBridge.nativeClose(dec) }, { NativeBridge.nativeClose(sub) })
                    return MountedVolume(v, closers, j) to j
                } catch (x: NativeException) {
                    NativeBridge.nativeClose(dec); NativeBridge.nativeClose(sub)
                    j.put("ok", false).put("errno", x.code).put("errName", errName(x.code)).put("message", "解錠後の FS マウントに失敗: ${x.message}").put("unlocked", info.format).put("mountMs", ms())
                    return null to j
                }
            } catch (x: Throwable) {
                if (Handle.valid(sub)) runCatching { NativeBridge.nativeClose(sub) }
                j.put("ok", false).put("errno", e.code).put("errName", errName(e.code)).put("message", "${e.message} / 暗号判定で例外 ${x.javaClass.simpleName}: ${x.message}").put("mountMs", ms())
                return null to j
            }
        } catch (e: Throwable) {
            j.put("ok", false).put("message", "${e.javaClass.simpleName}: ${e.message}").put("mountMs", ms())
            return null to j
        }
    }

    fun partitionJson(p: Partition) = JSONObject().apply {
        put("index", p.index); put("name", p.name); put("type", p.typeName); put("fs", p.fs); put("startBytes", p.startBytes); put("sizeBytes", p.sizeBytes)
        put("logical", p.logical)
    }

    /** 検証対象のパーティション (拡張コンテナ・保護 MBR は除く)。[only] はパーティション番号の指定 (空なら全部)。 */
    fun targetPartitions(pt: PartitionTable, only: List<String>): List<Partition> =
        pt.partitions.filter { !it.extendedContainer && !it.protective }.filter { only.isEmpty() || it.index.toString() in only }

    fun writeReport(ctx: Context, tag: String, json: JSONObject): File {
        val f = File(ctx.filesDir, "real_usb_report_$tag.json")
        f.writeText(json.toString(2))
        log("report written: ${f.absolutePath}")
        return f
    }
}
