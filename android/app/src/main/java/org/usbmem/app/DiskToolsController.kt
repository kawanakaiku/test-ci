// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.system.Os
import org.json.JSONArray
import org.json.JSONObject
import org.usbmem.app.ui.AppModel
import org.usbmem.app.ui.DiskOpJob
import org.usbmem.app.ui.DiskRange
import org.usbmem.app.ui.DiskTarget
import org.usbmem.app.ui.DiskTargetKind
import org.usbmem.app.ui.BootDone
import org.usbmem.app.ui.DiskToolsActions
import org.usbmem.app.ui.FormatCap
import org.usbmem.app.ui.FormatDone
import org.usbmem.app.ui.FormatSpec
import org.usbmem.app.ui.OpProgress
import org.usbmem.app.ui.Outcome
import org.usbmem.app.ui.PartOp
import org.usbmem.app.ui.PartitionDone
import org.usbmem.app.ui.RescueSpec
import org.usbmem.app.ui.ScanProposal
import org.usbmem.app.ui.ScanReport
import org.usbmem.app.ui.ScanSpec
import java.io.File
import java.security.MessageDigest

/** 利用者に見せる文言をもつ失敗 (画面は [Outcome.error] として出す)。 */
internal class DiskUiError(message: String) : Exception(message)

/**
 * ディスクツールの実処理 (検査 / フォーマット / パーティション編集 / バックアップ・復元 / 救出)。docs/disk-ui.md
 *
 * - 対象 ([DiskTarget]) は USB のセッション (UsbSession.withHandle) かイメージ (SAF のファイルを開く)。**呼び出しのたびに開いて、終わったら閉じる**
 *   (デバイスのロックを利用者の考えている間に持ち続けない)。「計画 → 確認 → 適用」の適用は、計画と同じ指定で計画を作り直して、確認文字列が
 *   画面に見せたものと一致することを確かめてから行う (計画は決定的: seed / 時刻 / GUID を画面が決めて持ち回る)。
 * - 破壊的な操作 (フォーマット・パーティション編集・復元) は、書き込みモード「すべて許可」と、マウント中でないことをここでも確かめる (画面の無効化に頼らない)。
 * - 救出の読み取り元・検査・バックアップの読み取り側には一切書かない (読み取り専用で開く)。
 * - 書く前の控え (mkfs・パーティション表) と救出の進捗ファイル (mapfile) は、アプリ内の files/disktools に置く。
 */
class DiskToolsController(private val ctx: Context, private val model: AppModel) : DiskToolsActions {
    private fun s(id: Int, vararg a: Any) = ctx.getString(id, *a)
    private val root: File get() = File(ctx.filesDir, "disktools").also { it.mkdirs() }
    private fun sub(name: String) = File(root, name).also { it.mkdirs() }
    private fun nowSec() = System.currentTimeMillis() / 1000

    private val main = Handler(Looper.getMainLooper())

    // ================================================================ 共通

    private fun <T> guard(block: () -> T): Outcome<T> = try {
        Outcome(value = block())
    } catch (e: DiskUiError) {
        Outcome(error = e.message ?: "")
    } catch (e: DiskToolsException) {
        Outcome(error = failText(e.detail, e.message))
    } catch (e: PartitionEditException) {
        Outcome(error = failText(e.detail, e.message))
    } catch (e: PartitionScanException) {
        Outcome(error = failText(e.detail, e.message))
    } catch (e: RecoveryException) {
        Outcome(error = failText(e.code, e.message))
    } catch (e: NativeException) {
        Outcome(error = s(R.string.dt_err_native, NativeBridge.nativeErrcName(e.code)))
    } catch (e: java.io.FileNotFoundException) {
        Outcome(error = s(R.string.dt_err_file_open))
    } catch (e: SecurityException) {
        Outcome(error = s(R.string.dt_err_file_open))
    } catch (e: Exception) {
        Outcome(error = s(R.string.dt_err_unexpected, e.javaClass.simpleName))
    }

    private fun failText(detail: String, message: String?): String {
        val known = org.usbmem.app.ui.diskCodeLabel(ctx, detail)
        return if (known != detail) known else (message ?: detail)
    }

    private fun usbState(t: DiskTarget) = model.device(t.key)

    override fun writeAllowed(t: DiskTarget): Boolean = BuildConfig.WRITE_SUPPORTED && when (t.kind) {
        DiskTargetKind.Usb -> usbState(t)?.let { it.open && it.writable && it.writeMode == WriteMode.Full } ?: false
        DiskTargetKind.Image -> t.imageWritable
    }

    override fun inUse(t: DiskTarget, r: DiskRange): Boolean = when (t.kind) {
        DiskTargetKind.Usb -> usbState(t)?.let { d ->
            d.osBlocked > 0 || d.published.any { p -> r.whole || (p.startBytes >= r.start && p.startBytes < r.start + r.size) }
        } ?: false
        // イメージは、マウント中ならどの範囲でも使用中とみなす (範囲ごとの対応は持たない)
        DiskTargetKind.Image -> VolumeRegistry.all(ctx).any { it.sourceUri == t.key }
    }

    private fun wholeRange() = DiskRange(0, Long.MAX_VALUE / 2, "", "", 0)

    private fun requireWritable(t: DiskTarget, r: DiskRange) {
        if (!writeAllowed(t)) throw DiskUiError(s(R.string.dt_err_not_writable))
        if (inUse(t, r)) throw DiskUiError(s(R.string.dt_err_in_use))
    }

    /** [t] のデバイスのハンドルを、ブロックの間だけ渡す。[write] = true なら書き込み可で開いたもの (書き込みモードが許さなければ拒否)。 */
    private fun <T> withDevice(t: DiskTarget, write: Boolean, block: (Long) -> T): T {
        if (write && !writeAllowed(t)) throw DiskUiError(s(R.string.dt_err_not_writable))
        return when (t.kind) {
            DiskTargetKind.Usb -> {
                val sess = UsbSessions.manager(ctx).session(t.key) ?: throw DiskUiError(s(R.string.dt_err_device_gone))
                if (write && !sess.writable) throw DiskUiError(s(R.string.dt_err_not_writable))
                sess.withHandle(block)
            }
            DiskTargetKind.Image -> {
                val uri = Uri.parse(t.key)
                val pfd = ctx.contentResolver.openFileDescriptor(uri, if (write) "rw" else "r") ?: throw DiskUiError(s(R.string.dt_err_file_open))
                val h = NativeBridge.nativeOpenFd(pfd.detachFd(), !write)
                if (!Handle.valid(h)) throw DiskUiError(s(R.string.dt_err_native, NativeBridge.nativeErrcName((-h).toInt())))
                try { block(h) } finally { NativeBridge.nativeClose(h) }
            }
        }
    }

    /** [r] がパーティション (開始位置が 0 でない、または全体より小さい) なら切り出したデバイスを、全体なら [h] そのものを渡す (フォーマットしたいのは範囲の先頭から始まるデバイス)。 */
    private fun <T> withRange(h: Long, r: DiskRange, write: Boolean, block: (Long) -> T): T {
        if (r.whole && r.start == 0L) return block(h)
        val sub = NativeBridge.nativeDeviceSubRangeMode(h, r.start, r.size, !write)
        if (!Handle.valid(sub)) throw DiskUiError(s(R.string.dt_err_native, NativeBridge.nativeErrcName((-sub).toInt())))
        try { return block(sub) } finally { NativeBridge.nativeClose(sub) }
    }

    override fun readTable(t: DiskTarget): Outcome<PartitionTable> = guard {
        withDevice(t, false) { h ->
            val err = IntArray(1)
            val b = NativeBridge.nativeListPartitions(h, err) ?: throw NativeException(err[0], "partition scan failed")
            PartitionTable.parse(String(b, Charsets.UTF_8))
        }
    }

    // ================================================================ ジョブ

    private class CheckJob : DiskOpJob {
        val job = DiskJob()
        private val lock = Any()
        private var closed = false
        override fun cancel() = synchronized(lock) { if (!closed) job.cancel() }
        override fun pause(paused: Boolean) {}
        override fun progress(): OpProgress? = synchronized(lock) {
            if (closed) null else job.progress().let { OpProgress(it.phase, it.done, it.total) }
        }
        override fun close() = synchronized(lock) { if (!closed) { closed = true; job.close() } }
    }

    private class RecJob : DiskOpJob {
        val job = RecoveryJob()
        private val lock = Any()
        private var closed = false
        override fun cancel() = synchronized(lock) { if (!closed) job.cancel() }
        override fun pause(paused: Boolean) = synchronized(lock) { if (!closed) job.pause(paused) }
        override fun progress(): OpProgress? = synchronized(lock) {
            if (closed) null else job.progress()?.let { j ->
                val phase = j.optString("phase")
                // 救出の進捗は pos / size (入力全体)。バックアップ・復元は done / total
                val total = if (j.optString("total", "0") != "0") j.optString("total", "0").toLongOrNull() ?: 0L else j.optString("size", "0").toLongOrNull() ?: 0L
                val done = if (j.optString("total", "0") != "0") j.optString("done", "0").toLongOrNull() ?: 0L else j.optString("pos", "0").toLongOrNull() ?: 0L
                OpProgress(phase, done, total, j)
            }
        }
        override fun close() = synchronized(lock) { if (!closed) { closed = true; job.close() } }
    }

    /** パーティション探索・ブートセクタの検査 / 修復のジョブ (ScanJob)。 */
    private class ScanJobImpl : DiskOpJob {
        val job = ScanJob()
        private val lock = Any()
        private var closed = false
        override fun cancel() = synchronized(lock) { if (!closed) job.cancel() }
        override fun pause(paused: Boolean) {}
        override fun progress(): OpProgress? = synchronized(lock) {
            if (closed) null else job.progress().let { OpProgress(it.phase, it.done, it.total) }
        }
        override fun close() = synchronized(lock) { if (!closed) { closed = true; job.close() } }
    }

    override fun newCheckJob(): DiskOpJob = CheckJob()
    override fun newScanJob(): DiskOpJob = ScanJobImpl()
    private fun scanJob(job: DiskOpJob) = (job as ScanJobImpl).job
    override fun newRecoveryJob(): DiskOpJob = RecJob()
    private fun disk(job: DiskOpJob) = (job as CheckJob).job
    private fun rec(job: DiskOpJob) = (job as RecJob).job

    // ================================================================ 検査 (読み取り専用)

    override fun check(t: DiskTarget, r: DiskRange, job: DiskOpJob): Outcome<FsCheckResult> = guard {
        withDevice(t, false) { h -> DiskCheck.check(h, r.start, if (r.whole) 0 else r.size, null, 0, 0, disk(job)) }
    }

    // ================================================================ フォーマット

    override fun formatCaps(r: DiskRange, sectorSize: Int): List<FormatCap> = try {
        val ss = if (sectorSize in 512..4096) sectorSize else 512
        val base = MkfsSession.capabilities(r.size, ss, 0)
        fun sizeOkFor(fs: String, cluster: Int): Boolean {
            val arr = MkfsSession.capabilities(r.size, ss, cluster)
            for (i in 0 until arr.length()) { val o = arr.getJSONObject(i); if (o.optString("fs") == fs) return o.optBoolean("sizeOk") }
            return false
        }
        List(base.length()) { i ->
            val o = base.getJSONObject(i)
            val fs = o.getString("fs")
            val sizes = o.optJSONArray("clusterSizes")
            val cs = if (sizes == null) emptyList() else List(sizes.length()) { k -> sizes.getInt(k) }
            val feats = o.optJSONArray("features")
            val supported = o.optBoolean("supported")
            val sizeOk = o.optBoolean("sizeOk", true)
            FormatCap(
                fs = fs, supported = supported, experimental = o.optBoolean("experimental"),
                minBytes = o.optString("minBytes", "0").toLongOrNull() ?: 0L, maxBytes = o.optString("maxBytes", "0").toLongOrNull() ?: 0L,
                clusterSizes = cs, clusterOk = if (supported && sizeOk) cs.filter { c -> sizeOkFor(fs, c) } else emptyList(),
                defaultCluster = o.optInt("defaultCluster"), labelMax = o.optInt("labelMax"), labelCharset = o.optString("labelCharset"),
                features = if (feats == null) emptyList() else List(feats.length()) { k -> feats.getString(k) },
                sizeOk = sizeOk, reasonCode = o.optString("reasonCode"), reason = o.optString("reason"),
            )
        }.filter { it.supported }
    } catch (_: Exception) { emptyList() }

    private fun openMkfs(h: Long, r: DiskRange, spec: FormatSpec): MkfsSession {
        if (spec.features.isBlank()) return MkfsSession.open(h, spec.fs, r.start, r.size, spec.label.ifBlank { null }, spec.clusterSize, 0, 0, 0, false, spec.seed, spec.timestampSec)
        val opts = LinkedHashMap<String, String>()
        if (spec.label.isNotBlank()) opts["label"] = spec.label
        if (spec.clusterSize > 0) opts["cluster_size"] = spec.clusterSize.toString()
        opts["features"] = spec.features
        return MkfsSession.openEx(h, spec.fs, r.start, r.size, opts, false, spec.seed, spec.timestampSec)
    }

    override fun formatPlan(t: DiskTarget, r: DiskRange, spec: FormatSpec): Outcome<MkfsPlan> = guard {
        withDevice(t, writeAllowed(t)) { h -> openMkfs(h, r, spec.copy()).use { it.plan() } }
    }

    override fun formatApply(t: DiskTarget, r: DiskRange, spec: FormatSpec, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<FormatDone> = guard {
        requireWritable(t, r)
        val ts = nowSec()
        val backup = File(sub("backups"), "mkfs-$ts-${r.index}.bak")
        withDevice(t, true) { h ->
            openMkfs(h, r, spec).use { m ->
                val plan = m.plan()
                if (plan.confirmPhrase != expectedPhrase) throw DiskUiError(s(R.string.dt_err_plan_changed))
                val res = m.apply(typed, backup.path, false, disk(job), ts)
                FormatDone(res, backup.name.takeIf { backup.exists() })
            }
        }
    }

    // ================================================================ パーティション編集

    private fun sectorOf(session: PartitionEditSession): Int = session.tableJson().optInt("sectorSize", 512)

    private fun readBytes(uri: Uri): ByteArray {
        val ins = ctx.contentResolver.openInputStream(uri) ?: throw DiskUiError(s(R.string.dt_err_file_open))
        ins.use { i ->
            val out = java.io.ByteArrayOutputStream()
            val buf = ByteArray(65536)
            while (true) {
                val n = i.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
                if (out.size() > (32 shl 20)) throw DiskUiError(s(R.string.dt_err_file_too_big))
            }
            return out.toByteArray()
        }
    }

    /** 操作を新しいセッションへ流して、計画を作る。呼び出し元が [PartitionEditSession.close] すること。 */
    private fun buildSession(h: Long, ops: List<PartOp>, inUse: Boolean): Pair<PartitionEditSession, PartitionPlan> {
        val sess = PartitionEditSession.open(h, inUse)
        try {
            val ss = sectorOf(sess)
            var gpt = sess.tableJson().optString("kind") == "gpt"
            var restore: PartOp.RestoreTable? = null
            for (op in ops) when (op) {
                is PartOp.NewTable -> { sess.newTable(op.gpt, 128, op.overwriteFs); sess.setDiskId(op.diskId); gpt = op.gpt }
                is PartOp.Create -> sess.create(
                    0, 0, op.sizeBytes / ss, op.mbrType, if (gpt) op.gptType else null, if (gpt) op.uniqueGuid else null,
                    if (gpt) op.name.ifBlank { null } else null, 0, false,
                )
                is PartOp.Remove -> sess.remove(op.index)
                is PartOp.Resize -> sess.resize(op.index, op.sizeBytes / ss)
                is PartOp.Rename -> sess.setName(op.index, op.name)
                is PartOp.SetType -> if (gpt) sess.setGptType(op.index, op.gptType) else sess.setMbrType(op.index, op.mbrType)
                is PartOp.Repair -> sess.repairGpt(
                    when (op.kind) {
                        "copy-primary-to-backup" -> PartitionEditSession.GptRepair.CopyPrimaryToBackup
                        "copy-backup-to-primary" -> PartitionEditSession.GptRepair.CopyBackupToPrimary
                        "move-backup-to-end" -> PartitionEditSession.GptRepair.MoveBackupToEnd
                        else -> PartitionEditSession.GptRepair.RebuildProtectiveMbr
                    },
                )
                is PartOp.RestoreTable -> restore = op
            }
            val plan = if (restore != null) sess.planRestore(readBytes(restore.uri)) else sess.plan()
            return sess to plan
        } catch (e: Throwable) {
            sess.close()
            throw e
        }
    }

    override fun partitionGpt(t: DiskTarget): Outcome<JSONObject> = guard {
        withDevice(t, false) { h -> PartitionEditSession.open(h, false).use { it.gptInspection() } }
    }

    override fun partitionPlan(t: DiskTarget, ops: List<PartOp>): Outcome<PartitionPlan> = guard {
        withDevice(t, writeAllowed(t)) { h ->
            val (sess, plan) = buildSession(h, ops, inUse(t, wholeRange()))
            sess.close()
            plan
        }
    }

    override fun partitionApply(t: DiskTarget, ops: List<PartOp>, expectedPhrase: String, typed: String): Outcome<PartitionDone> = guard {
        requireWritable(t, wholeRange())
        val ts = nowSec()
        val backup = File(sub("backups"), "partition-$ts.bak")
        withDevice(t, true) { h ->
            val (sess, plan) = buildSession(h, ops, false)
            try {
                if (plan.confirmPhrase != expectedPhrase) throw DiskUiError(s(R.string.dt_err_plan_changed))
                val res = sess.apply(typed, backup.path, ts)
                var after: PartitionTable? = null
                if (res.ok) {
                    sess.close()   // 表を読み直す前に、編集セッションを閉じる (ハンドルを閉じる前に閉じる規則)
                    val err = IntArray(1)
                    NativeBridge.nativeListPartitions(h, err)?.let { after = PartitionTable.parse(String(it, Charsets.UTF_8)) }
                    val tbl = after
                    if (t.kind == DiskTargetKind.Usb && tbl != null) main.post { usbState(t)?.table = tbl }
                }
                PartitionDone(res, backup.name.takeIf { backup.exists() }, after)
            } finally {
                sess.close()
            }
        }
    }

    override fun partitionBackupTo(t: DiskTarget, dest: Uri): Outcome<Int> = guard {
        val bytes = withDevice(t, false) { h -> PartitionEditSession.open(h, false).use { it.backup(nowSec()) } }
        val out = ctx.contentResolver.openOutputStream(dest, "wt") ?: throw DiskUiError(s(R.string.dt_err_file_open))
        out.use { it.write(bytes) }
        bytes.size
    }

    // ================================================================ 失われたパーティションを探す (探索は読み取り専用。docs/partition-scan.md)

    override fun scanRun(t: DiskTarget, spec: ScanSpec, job: DiskOpJob): Outcome<ScanReport> = guard {
        // 計画の識別子は開き方 (rw / ro) を含むので、適用と同じ開き方で計画を作る (探索そのものは何も書かない)
        withDevice(t, writeAllowed(t)) { h ->
            val sc = PartitionScanner.run(h, spec.full, 0, spec.minConfidence, scanJob(job))
            try {
                val result = sc.resultJson()
                if (sc.cancelled) {
                    ScanReport(result, true, emptyList())
                } else {
                    val props = sc.proposalsJson()
                    val list = ArrayList<ScanProposal>()
                    for (i in 0 until props.length()) {
                        val pj = props.getJSONObject(i)
                        var plan: PartitionPlan? = null
                        var err: String? = null
                        try {
                            PartitionEditSession.open(h, false, 0, spec.seed).use { ed ->
                                sc.queueProposal(pj.getInt("index"), ed)
                                plan = ed.plan()
                            }
                        } catch (e: PartitionEditException) {
                            err = failText(e.detail, e.message)
                        } catch (e: PartitionScanException) {
                            err = failText(e.detail, e.message)
                        }
                        list.add(ScanProposal(pj, plan, err))
                    }
                    ScanReport(result, false, list)
                }
            } finally {
                sc.close()
            }
        }
    }

    override fun scanApply(t: DiskTarget, spec: ScanSpec, proposalId: String, proposalIndex: Int, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<PartitionDone> = guard {
        requireWritable(t, wholeRange())
        val ts = nowSec()
        val backup = File(sub("backups"), "scan-$ts.bak")
        withDevice(t, true) { h ->
            val sc = PartitionScanner.run(h, spec.full, 0, spec.minConfidence, scanJob(job))
            try {
                if (sc.cancelled) throw DiskUiError(s(R.string.dt_code_cancelled))
                val props = sc.proposalsJson()
                var same = false
                for (i in 0 until props.length()) {
                    val pj = props.getJSONObject(i)
                    if (pj.getInt("index") == proposalIndex && pj.optString("id") == proposalId) same = true
                }
                if (!same) throw DiskUiError(s(R.string.dt_err_plan_changed))
                val ed = PartitionEditSession.open(h, false, 0, spec.seed)
                try {
                    sc.queueProposal(proposalIndex, ed)
                    val plan = ed.plan()
                    if (plan.confirmPhrase != expectedPhrase) throw DiskUiError(s(R.string.dt_err_plan_changed))
                    if (!plan.ok) throw DiskUiError(s(R.string.dt_plan_not_ok))
                    val res = ed.apply(typed, backup.path, ts)
                    var after: PartitionTable? = null
                    if (res.ok) {
                        ed.close()   // 表を読み直す前に、編集セッションを閉じる (ハンドルを閉じる前に閉じる規則)
                        val err = IntArray(1)
                        NativeBridge.nativeListPartitions(h, err)?.let { after = PartitionTable.parse(String(it, Charsets.UTF_8)) }
                        val tbl = after
                        if (t.kind == DiskTargetKind.Usb && tbl != null) main.post { usbState(t)?.table = tbl }
                    }
                    PartitionDone(res, backup.name.takeIf { backup.exists() }, after)
                } finally {
                    ed.close()
                }
            } finally {
                sc.close()
            }
        }
    }

    // ================================================================ ブートセクタの検査 (読み取り専用) / 修復

    private fun bootOffset(r: DiskRange) = if (r.whole) 0L else r.start
    private fun bootLength(r: DiskRange) = if (r.whole) 0L else r.size

    override fun bootInspect(t: DiskTarget, r: DiskRange, job: DiskOpJob): Outcome<JSONObject> = guard {
        withDevice(t, false) { h -> BootFixSession.open(h, bootOffset(r), bootLength(r), null, scanJob(job)).use { it.inspection() } }
    }

    override fun bootPlan(t: DiskTarget, r: DiskRange, repairId: String, source: Long): Outcome<JSONObject> = guard {
        // 計画の識別子は開き方 (rw / ro) を含むので、適用と同じ開き方で作る
        withDevice(t, writeAllowed(t)) { h -> BootFixSession.open(h, bootOffset(r), bootLength(r), null, null).use { it.plan(repairId, source) } }
    }

    override fun bootApply(t: DiskTarget, r: DiskRange, repairId: String, source: Long, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<BootDone> = guard {
        requireWritable(t, r)
        val ts = nowSec()
        val backup = File(sub("backups"), "boot-$ts-${r.index}.bak")
        withDevice(t, true) { h ->
            val res = BootFixSession.open(h, bootOffset(r), bootLength(r), null, null).use { b ->
                val plan = b.plan(repairId, source)
                if (!plan.optBoolean("ok")) throw DiskUiError(s(R.string.dt_plan_not_ok))
                if (plan.optString("confirmPhrase") != expectedPhrase) throw DiskUiError(s(R.string.dt_err_plan_changed))
                b.apply(typed, backup.path, false, scanJob(job), ts)
            }
            val after = try { BootFixSession.open(h, bootOffset(r), bootLength(r), null, null).use { it.inspection() } } catch (_: Exception) { null }
            BootDone(res, backup.name.takeIf { backup.exists() }, after)
        }
    }

    // ================================================================ バックアップ / 復元

    /** SAF のファイルを、ネイティブ (パスを取る API) へ渡すための /proc/self/fd のパスにする。[block] の間だけ有効。 */
    private fun <T> withFdPath(uri: Uri, mode: String, block: (String) -> T): T {
        val pfd = ctx.contentResolver.openFileDescriptor(uri, mode) ?: throw DiskUiError(s(R.string.dt_err_file_open))
        try { return block("/proc/self/fd/${pfd.fd}") } finally { try { pfd.close() } catch (_: Exception) { } }
    }

    private fun deleteQuietly(uri: Uri) {
        try {
            if (uri.scheme == "file") File(uri.path ?: return).delete()
            else android.provider.DocumentsContract.deleteDocument(ctx.contentResolver, uri)
        } catch (_: Exception) { }
    }

    override fun backupRun(t: DiskTarget, r: DiskRange, dest: Uri, lz4: Boolean, fullIfUnclean: Boolean, job: DiskOpJob): Outcome<JSONObject> = guard {
        if (inUse(t, r) && writableMount(t, r)) throw DiskUiError(s(R.string.dt_err_backup_mounted_rw))
        val res = withFdPath(dest, "rwt") { path ->
            withDevice(t, false) { h -> withRange(h, r, false) { d -> Backup.run(d, path, lz4, fullIfUnclean, rec(job)) } }
        }
        if (!res.optBoolean("ok")) deleteQuietly(dest)
        res
    }

    /** [r] のマウントが書き込み可か (書き込み可でマウント中のボリュームのバックアップは、内容が途中の状態になりうるので断る) */
    private fun writableMount(t: DiskTarget, r: DiskRange): Boolean = when (t.kind) {
        DiskTargetKind.Usb -> usbState(t)?.published?.any { p -> p.writable && (r.whole || (p.startBytes >= r.start && p.startBytes < r.start + r.size)) } ?: false
        DiskTargetKind.Image -> VolumeRegistry.all(ctx).any { it.sourceUri == t.key && it.volume.writable }
    }

    override fun imageInspect(src: Uri, verify: Boolean): Outcome<JSONObject> = guard {
        withFdPath(src, "r") { path -> Backup.inspect(path, verify) }
    }

    override fun restorePlan(t: DiskTarget, r: DiskRange, src: Uri): Outcome<JSONObject> = guard {
        val inUse = inUse(t, r)
        withFdPath(src, "r") { path ->
            withDevice(t, writeAllowed(t)) { h ->
                withRange(h, r, writeAllowed(t)) { d -> RestorePlan.create(path, d, inUse, true).use { it.json } }
            }
        }
    }

    override fun restoreApply(t: DiskTarget, r: DiskRange, src: Uri, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<JSONObject> = guard {
        requireWritable(t, r)
        withFdPath(src, "r") { path ->
            withDevice(t, true) { h ->
                withRange(h, r, true) { d ->
                    RestorePlan.create(path, d, false, true).use { plan ->
                        if (!plan.ok) throw DiskUiError(plan.message.ifBlank { s(R.string.dt_err_plan_changed) })
                        if (plan.confirmPhrase != expectedPhrase) throw DiskUiError(s(R.string.dt_err_plan_changed))
                        plan.apply(d, plan.token(typed), false, rec(job))
                    }
                }
            }
        }
    }

    // ================================================================ 救出

    private fun sha(x: String) = MessageDigest.getInstance("SHA-256").digest(x.toByteArray()).joinToString("") { "%02x".format(it) }

    private fun mapFile(t: DiskTarget, r: DiskRange, out: Uri) = File(sub("rescue"), sha("${t.kind}|${t.key}|${r.start}|${r.size}|$out").take(24) + ".map")

    override fun rescueResetMap(t: DiskTarget, r: DiskRange, out: Uri) { mapFile(t, r, out).delete(); File(mapFile(t, r, out).path + ".tmp").delete() }

    private fun options(r: DiskRange, spec: RescueSpec) = RescueOptions(
        start = if (r.whole) 0 else r.start, length = if (r.whole) 0 else r.size, outOffset = 0, clusterSectors = spec.clusterSectors, retries = spec.retries,
        noTrim = spec.noTrim, noScrape = spec.noScrape, pauseBetweenReadsMs = spec.pauseBetweenReadsMs,
        maxConsecutiveErrors = spec.maxConsecutiveErrors, noProgressTimeoutMs = spec.noProgressTimeoutSec * 1000L,
    )

    private fun fileId(uri: Uri): Pair<Long, Long>? = try {
        ctx.contentResolver.openFileDescriptor(uri, "r")?.use { p -> Os.fstat(p.fileDescriptor).let { it.st_dev to it.st_ino } }
    } catch (_: Exception) { null }

    /** 読み取り元と出力が同じファイルか (イメージを元にするとき)。同じなら出力で元を壊すので、必ず断る。 */
    private fun sameFile(t: DiskTarget, out: Uri): Boolean {
        if (t.kind != DiskTargetKind.Image) return false
        if (t.key == out.toString()) return true
        val a = fileId(Uri.parse(t.key)) ?: return false
        val b = fileId(out) ?: return false
        return a == b
    }

    /** 出力ファイルを、書き込み可のデバイスとして開く。必要な大きさに満たなければ (疎に) 伸ばす。[extend] = false なら伸ばさない (開始前の検査で、空の新しいファイルだけ伸ばす)。 */
    private fun <T> withOutput(out: Uri, need: Long, extend: Boolean, block: (Long, Long) -> T): T {
        val pfd = ctx.contentResolver.openFileDescriptor(out, "rw") ?: throw DiskUiError(s(R.string.dt_err_file_open))
        val before = try { Os.fstat(pfd.fileDescriptor).st_size } catch (_: Exception) { 0L }
        if (before < need && (extend || before == 0L)) {
            try { Os.ftruncate(pfd.fileDescriptor, need) } catch (_: Exception) { pfd.close(); throw DiskUiError(s(R.string.dt_err_rescue_extend)) }
        }
        val h = NativeBridge.nativeOpenFd(pfd.detachFd(), false)
        if (!Handle.valid(h)) throw DiskUiError(s(R.string.dt_err_native, NativeBridge.nativeErrcName((-h).toInt())))
        try { return block(h, before) } finally { NativeBridge.nativeClose(h) }
    }

    override fun rescuePreflight(t: DiskTarget, r: DiskRange, out: Uri, spec: RescueSpec): Outcome<JSONObject> = guard {
        if (sameFile(t, out)) throw DiskUiError(s(R.string.dt_err_rescue_same_file))
        val need = r.size
        withOutput(out, need, extend = false) { oh, before ->
            val res = withDevice(t, false) { ih -> Rescue.preflight(ih, oh, mapFile(t, r, out).path, options(r, spec)) }
            val warnings = JSONArray()
            if (res.optString("code") == "output-too-small" && before in 1 until need) {
                // 小さい既存ファイル: 実行すると疎に伸ばす (先頭は変えない)
                res.put("ok", true); res.put("code", "ok"); res.put("stop", "finished")
                warnings.put("extend")
            }
            if (res.optString("mapfile") == "resume" && before == 0L && res.optString("finished", "0") != "0") warnings.put("output-empty-but-map")
            res.put("outputBefore", before.toString())
            res.put("warnings", warnings)
            res
        }
    }

    override fun rescueRun(t: DiskTarget, r: DiskRange, out: Uri, spec: RescueSpec, job: DiskOpJob): Outcome<JSONObject> = guard {
        if (sameFile(t, out)) throw DiskUiError(s(R.string.dt_err_rescue_same_file))
        withOutput(out, r.size, extend = true) { oh, _ ->
            withDevice(t, false) { ih -> Rescue.run(ih, oh, mapFile(t, r, out).path, options(r, spec), rec(job)) }
        }
    }
}
