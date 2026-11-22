package org.usbmem.app

import org.json.JSONObject

/**
 * パーティション探索 (TestDisk 相当) / 失われた表の復元案 / ブートセクタの検査と修復のネイティブ入口 (jni_partscan.cpp)。UI は未接続 (core の API と JNI まで)。
 * 使うときは [PartitionScanner] (探索と復元案) と [BootFixSession] (ブートセクタ) 経由にする。設計と安全モデルは docs/partition-scan.md。
 *
 * 戻り値の約束は NativeBridge と同じ: 0 以外の Int は -(usbmem::Errc)、byte[] が null のときは errorOut[0] にコード。
 * 探索と検査は読み取り専用 (読み取り専用で開いたデバイスで使える)。書き込みは必ず 計画 → 確認トークン → 適用 の 3 段で、書き込み前にバックアップを取る。
 */
object PartitionScan {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeJobNew(): Long
    @JvmStatic external fun nativeJobFree(job: Long)
    @JvmStatic external fun nativeJobCancel(job: Long)
    /** 戻り値: 段階名 (UTF-8)。progress[0] = done, progress[1] = total */
    @JvmStatic external fun nativeJobProgress(job: Long, progress: LongArray): ByteArray

    /** 戻り値: ハンドル (>4095) / -(Errc)。キャンセルされても途中までの結果のハンドルが返り、outRc[0] = 5 (Cancelled) */
    @JvmStatic external fun nativeScanRun(dev: Long, sectorSize: Int, level: Int, minConfidence: Int, job: Long, outRc: IntArray): Long
    @JvmStatic external fun nativeScanClose(h: Long)
    @JvmStatic external fun nativeScanResultJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeScanProposalsJson(h: Long, err: IntArray): ByteArray?
    /** 案を PartitionEditor のハンドルに積む (既存の操作は捨てる)。書き込みはしない */
    @JvmStatic external fun nativeScanQueueProposal(h: Long, index: Int, editor: Long): Int

    @JvmStatic external fun nativeBootOpen(dev: Long, offset: Long, length: Long, fsName: ByteArray?, job: Long): Long
    @JvmStatic external fun nativeBootClose(h: Long)
    @JvmStatic external fun nativeBootLastError(h: Long): ByteArray
    @JvmStatic external fun nativeBootInspectionJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBootPlanJson(h: Long, repairId: ByteArray, source: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBootMakeToken(h: Long, typed: ByteArray, err: IntArray): ByteArray?
    /** 失敗しても結果 JSON (code / message / rolledBack ...) が返る。err[0] は戻り値 (0 = 成功) */
    @JvmStatic external fun nativeBootApply(h: Long, token: ByteArray, backupPath: ByteArray?, timestamp: Long, inUse: Boolean, job: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBootLastBackup(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBootBackupInfo(data: ByteArray, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBootRestoreBackup(dev: Long, data: ByteArray): Int
}

class PartitionScanException(val code: Int, val detail: String, message: String) : Exception(message)

/** 探索 / 修復の進捗とキャンセル。処理が終わってから [close]。[cancel] と [progress] は別スレッドから呼んでよい */
class ScanJob : AutoCloseable {
    private var h: Long = PartitionScan.nativeJobNew()
    data class Progress(val phase: String, val done: Long, val total: Long)

    internal fun handle(): Long = h
    fun cancel() { if (Handle.valid(h)) PartitionScan.nativeJobCancel(h) }
    fun progress(): Progress {
        if (!Handle.valid(h)) return Progress("", 0, 0)
        val p = LongArray(2)
        val phase = String(PartitionScan.nativeJobProgress(h, p), Charsets.UTF_8)
        return Progress(phase, p[0], p[1])
    }
    override fun close() {
        if (Handle.valid(h)) PartitionScan.nativeJobFree(h)
        h = 0
    }
}

/**
 * パーティション探索の結果。[resultJson] は候補の一覧 (candidates[]: fs / startLba / sizeBytes / confidence / relation ...)、
 * [proposalsJson] は表の復元案 (信頼度順)。[queueProposal] で案を [PartitionEditSession] に積み、あとは
 * PartitionEditSession.plan → apply (確認文字列の入力・書き込み前のバックアップ・読み戻し検証)。
 */
class PartitionScanner private constructor(private var h: Long, val cancelled: Boolean) : AutoCloseable {
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    private fun text(what: String, call: (IntArray) -> ByteArray?): String {
        val err = IntArray(1)
        val b = call(err) ?: throw PartitionScanException(err[0], what, "$what failed: ${NativeBridge.nativeErrcName(err[0])}")
        return String(b, Charsets.UTF_8)
    }

    fun resultJson(): JSONObject = JSONObject(text("scan result") { PartitionScan.nativeScanResultJson(live(), it) })

    /** 復元案 ({"index","id","title","table","action","confidence","planOk","confirmPhrase","notes","problems","changes","partitions"} の配列)。初回は計算する */
    fun proposalsJson(): org.json.JSONArray = org.json.JSONArray(text("proposals") { PartitionScan.nativeScanProposalsJson(live(), it) })

    /** 案 ([proposalsJson] の index) の操作を編集セッションに積む。書き込みはしない */
    fun queueProposal(index: Int, editor: PartitionEditSession) {
        val rc = PartitionScan.nativeScanQueueProposal(live(), index, editor.nativeHandle())
        if (rc != 0) throw PartitionScanException(-rc, "queue", "queue proposal failed: ${NativeBridge.nativeErrcName(-rc)}")
    }

    override fun close() {
        if (Handle.valid(h)) PartitionScan.nativeScanClose(h)
        h = 0
    }

    companion object {
        /**
         * 探索する (読み取り専用)。[full] = false: 典型的な位置だけ (速い) / true: 全セクタ。sectorSize 0 = 自動。キャンセルされると cancelled = true で途中までの結果を返す。
         * 必ずデバイスを閉じる前に [close] すること。
         */
        fun run(deviceHandle: Long, full: Boolean = false, sectorSize: Int = 0, minConfidence: Int = 0, job: ScanJob? = null): PartitionScanner {
            val rc = IntArray(1)
            val h = PartitionScan.nativeScanRun(deviceHandle, sectorSize, if (full) 1 else 0, minConfidence, job?.handle() ?: 0L, rc)
            if (Handle.isError(h)) throw PartitionScanException((-h).toInt(), "scan", "scan failed: ${NativeBridge.nativeErrcName((-h).toInt())}")
            return PartitionScanner(h, rc[0] != 0)
        }
    }
}

/** ブートセクタの修復の適用結果 */
data class BootApplyResult(val ok: Boolean, val code: String, val message: String, val writesDone: Int, val rolledBack: Boolean, val verified: Boolean)

/**
 * 1 つのファイルシステムのブートセクタ / スーパーブロックの検査と修復。流れ: [inspection] → [plan] → (ユーザーが confirmPhrase を入力) → [apply]。
 * 検査は書き込まない。修復は書き込み前にバックアップを取り、書いた後に読み戻して再検査し、失敗したら元に戻す。
 * 必ずデバイスを閉じる**前**に [close] すること。
 */
class BootFixSession private constructor(private var h: Long) : AutoCloseable {
    private fun s(x: String) = x.toByteArray(Charsets.UTF_8)
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    private fun fail(rc: Int, what: String): Nothing {
        val parts = String(PartitionScan.nativeBootLastError(h), Charsets.UTF_8).split('\n', limit = 2)
        val detail = parts.getOrElse(0) { "" }
        throw PartitionScanException(rc, detail, "$what failed: ${parts.getOrElse(1) { "" }.ifEmpty { NativeBridge.nativeErrcName(rc) }} ($detail)")
    }

    private fun text(what: String, call: (IntArray) -> ByteArray?): String {
        val err = IntArray(1)
        val b = call(err) ?: fail(err[0], what)
        return String(b, Charsets.UTF_8)
    }

    /** 検査結果 (fs / verdict / findings[] / repairs[] / details / proposedBpb ...)。壊れていても例外にはならない */
    fun inspection(): JSONObject = JSONObject(text("inspect") { PartitionScan.nativeBootInspectionJson(live(), it) })

    /** 修復の計画 (書かない)。[repairId] は inspection の repairs[].id、[source] は ext の予備スーパーブロックのブロック番号 (0 = 最初のもの)。ok = false なら problems に理由 */
    fun plan(repairId: String, source: Long = 0): JSONObject = JSONObject(text("plan") { PartitionScan.nativeBootPlanJson(live(), s(repairId), source, it) })

    /**
     * 直近の計画を適用する。[typedPhrase] はユーザーが入力した文字列 (計画の confirmPhrase "REPAIR xxxxxx" と一致しなければ書かずに拒否)。
     * [backupPath] に書き込み前の内容を保存してから書く (保存できなければ何も書かない)。失敗しても例外ではなく結果を返す。
     */
    fun apply(typedPhrase: String, backupPath: String?, inUse: Boolean = false, job: ScanJob? = null,
              timestampSec: Long = System.currentTimeMillis() / 1000): BootApplyResult {
        val e = IntArray(1)
        val tok = PartitionScan.nativeBootMakeToken(live(), s(typedPhrase), e) ?: fail(e[0], "token")
        val b = PartitionScan.nativeBootApply(live(), tok, backupPath?.let(::s), timestampSec, inUse, job?.handle() ?: 0L, e) ?: fail(e[0], "apply")
        val j = JSONObject(String(b, Charsets.UTF_8))
        return BootApplyResult(j.getBoolean("ok"), j.getString("code"), j.optString("message"), j.optInt("writesDone"), j.optBoolean("rolledBack"), j.optBoolean("verified"))
    }

    /** 直近の apply が書き込む前に作ったバックアップ (backupPath を渡さなかったときの保険) */
    fun lastBackup(): ByteArray? {
        val e = IntArray(1)
        return PartitionScan.nativeBootLastBackup(live(), e)
    }

    override fun close() {
        if (Handle.valid(h)) PartitionScan.nativeBootClose(h)
        h = 0
    }

    companion object {
        /**
         * [deviceHandle] の [offset, offset + length) (length 0 = 終わりまで) を検査する。fsName: null = 自動判別 / "fat32" "exfat" "ntfs" "ext4" など。
         * 書き込みたいなら書き込み可で開いたデバイスを渡す (検査だけなら読み取り専用でよい)。
         */
        fun open(deviceHandle: Long, offset: Long = 0, length: Long = 0, fsName: String? = null, job: ScanJob? = null): BootFixSession {
            val h = PartitionScan.nativeBootOpen(deviceHandle, offset, length, fsName?.toByteArray(Charsets.UTF_8), job?.handle() ?: 0L)
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                val parts = String(PartitionScan.nativeBootLastError(0), Charsets.UTF_8).split('\n', limit = 2)
                throw PartitionScanException(code, parts.getOrElse(0) { "" }, "open failed: ${NativeBridge.nativeErrcName(code)} (${parts.getOrElse(0) { "" }})")
            }
            return BootFixSession(h)
        }

        /** バックアップファイルの中身を検査する (version / fs / timestamp / offset / length / records)。壊れていれば例外 */
        fun backupInfo(data: ByteArray): JSONObject {
            val e = IntArray(1)
            val b = PartitionScan.nativeBootBackupInfo(data, e) ?: throw PartitionScanException(e[0], "backup-corrupt", "not a valid boot-sector backup")
            return JSONObject(String(b, Charsets.UTF_8))
        }

        /** バックアップを書き戻す ([deviceHandle] は書き込み可で開いたもの)。失敗すると例外 */
        fun restoreBackup(deviceHandle: Long, data: ByteArray) {
            val rc = PartitionScan.nativeBootRestoreBackup(deviceHandle, data)
            if (rc != 0) throw PartitionScanException(-rc, "restore", "restore failed: ${NativeBridge.nativeErrcName(-rc)}")
        }
    }
}
