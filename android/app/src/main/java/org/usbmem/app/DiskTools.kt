package org.usbmem.app

import org.json.JSONArray
import org.json.JSONObject

/**
 * DiskTools (整合性検査 = fsck 相当 / mkfs) のネイティブ入口 (jni_disktools.cpp)。UI は未接続 (core の API と JNI まで)。
 * 使うときは [DiskCheck] (検査) と [MkfsSession] (作成) 経由にする。設計と安全モデルは docs/disk-tools.md。
 *
 * 戻り値の約束は NativeBridge と同じ: 0 以外の Int は -(usbmem::Errc)、byte[] が null のときは errorOut[0] にコード。
 */
object DiskTools {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeJobNew(): Long
    @JvmStatic external fun nativeJobFree(job: Long)
    @JvmStatic external fun nativeJobCancel(job: Long)
    /** 戻り値: 段階名 (UTF-8)。progress[0] = done, progress[1] = total */
    @JvmStatic external fun nativeJobProgress(job: Long, progress: LongArray): ByteArray

    /** 検査 (読み取り専用)。キャンセルされても JSON (verdict = incomplete) が返る。err[0] = Errc の値 (0 = 成功 / 5 = Cancelled) */
    @JvmStatic external fun nativeCheck(dev: Long, offset: Long, length: Long, fsName: ByteArray?, maxFindings: Int, memoryLimit: Long, job: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeCheckSupported(fsName: ByteArray): Boolean

    @JvmStatic external fun nativeMkfsOpen(dev: Long, fsName: ByteArray, offset: Long, length: Long, label: ByteArray?, clusterSize: Int, sectorSize: Int,
                                           inodeRatio: Int, inodeSize: Int, inUse: Boolean, seed: Long, timestamp: Long): Long
    @JvmStatic external fun nativeMkfsClose(h: Long)
    @JvmStatic external fun nativeMkfsLastError(h: Long): ByteArray
    @JvmStatic external fun nativeMkfsPlanJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeMkfsMakeToken(h: Long, typed: ByteArray, err: IntArray): ByteArray?
    /** 失敗しても結果 JSON (code / message / rolledBack ...) が返る。err[0] は戻り値 (0 = 成功) */
    @JvmStatic external fun nativeMkfsApply(h: Long, token: ByteArray, backupPath: ByteArray?, timestamp: Long, inUse: Boolean, job: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeMkfsMinSize(fsName: ByteArray, sectorSize: Int): Long
    @JvmStatic external fun nativeMkfsLastBackup(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeMkfsBackupInfo(data: ByteArray, err: IntArray): ByteArray?
    @JvmStatic external fun nativeMkfsRestore(dev: Long, data: ByteArray): Int
}

class DiskToolsException(val code: Int, val detail: String, message: String) : Exception(message)

/** 長い処理の進捗とキャンセル。処理が終わってから [close]。[cancel] と [progress] は別スレッドから呼んでよい */
class DiskJob : AutoCloseable {
    private var h: Long = DiskTools.nativeJobNew()
    data class Progress(val phase: String, val done: Long, val total: Long)

    internal fun handle(): Long = h
    fun cancel() { if (Handle.valid(h)) DiskTools.nativeJobCancel(h) }
    fun progress(): Progress {
        val p = LongArray(2)
        val ph = DiskTools.nativeJobProgress(h, p)
        return Progress(String(ph, Charsets.UTF_8), p[0], p[1])
    }
    override fun close() {
        if (Handle.valid(h)) DiskTools.nativeJobFree(h)
        h = 0
    }
}

enum class FsSeverity { Info, Warning, Error }

data class FsFinding(val severity: FsSeverity, val code: String, val message: String, val offset: Long?, val obj: String?)

/** 検査の結果。[verdict] は clean / warnings / errors / unsupported / incomplete */
data class FsCheckResult(
    val fs: String,
    val verdict: String,
    val incomplete: Boolean,
    val unclean: Boolean,
    val needsJournalRecovery: Boolean,
    val errors: Long,
    val warnings: Long,
    val infos: Long,
    val findings: List<FsFinding>,
    val findingsTruncated: Boolean,
    val json: JSONObject,
)

object DiskCheck {
    private fun s(x: String) = x.toByteArray(Charsets.UTF_8)

    /** 検査できる形式か (fat12 fat16 fat32 exfat ext2 ext3 ext4 ntfs) */
    fun supported(fsName: String): Boolean = DiskTools.nativeCheckSupported(s(fsName))

    /**
     * [deviceHandle] (NativeBridge が返したデバイスのハンドル) の [offset, offset+length) を検査する。書き込まない・修復しない。
     * 読み取り専用で開いたデバイスでよい。[fsName] = null は自動判別。[length] = 0 はデバイスの終わりまで。
     * キャンセルされた場合も例外にはせず、それまでの結果 (verdict = incomplete) を返す。
     */
    fun check(deviceHandle: Long, offset: Long = 0, length: Long = 0, fsName: String? = null, maxFindings: Int = 0, memoryLimit: Long = 0, job: DiskJob? = null): FsCheckResult {
        val err = IntArray(1)
        val b = DiskTools.nativeCheck(deviceHandle, offset, length, fsName?.let(::s), maxFindings, memoryLimit, job?.handle() ?: 0L, err)
            ?: throw DiskToolsException(err[0], "", "check failed: ${NativeBridge.nativeErrcName(err[0])}")
        return parse(String(b, Charsets.UTF_8))
    }

    internal fun parse(json: String): FsCheckResult {
        val o = JSONObject(json)
        return FsCheckResult(
            o.getString("fs"), o.getString("verdict"), o.optBoolean("incomplete"), o.optBoolean("unclean"), o.optBoolean("needsJournalRecovery"),
            o.optString("errors", "0").toLong(), o.optString("warnings", "0").toLong(), o.optString("infos", "0").toLong(),
            findings(o.optJSONArray("findings")), o.optBoolean("truncated"), o,
        )
    }

    private fun findings(a: JSONArray?): List<FsFinding> = if (a == null) emptyList() else List(a.length()) { i ->
        val f = a.getJSONObject(i)
        val sev = when (f.optString("severity")) { "error" -> FsSeverity.Error; "warning" -> FsSeverity.Warning; else -> FsSeverity.Info }
        FsFinding(sev, f.getString("code"), f.optString("message"), if (f.has("offset")) f.optString("offset").toLongOrNull() else null, f.optString("object").ifEmpty { null })
    }
}

/** mkfs の結果 (apply の JSON)。[verified] = 書いた後に読み戻して計画どおりで、整合性検査も clean だった */
data class MkfsApplyResult(
    val ok: Boolean,
    val code: String,
    val message: String,
    val bytesWritten: Long,
    val rolledBack: Boolean,
    val verified: Boolean,
    val checkVerdict: String,
)

/** 計画 (UI が差分と確認文字列を見せるためのもの)。[json] は全体 (stages / layout を含む) */
data class MkfsPlan(
    val ok: Boolean,
    val fs: String,
    val existingFs: String,
    val requiresForce: Boolean,
    val confirmPhrase: String,
    val planHash: String,
    val bytesToWrite: Long,
    val changes: List<String>,
    val problems: List<String>,
    val layout: Map<String, String>,
    val json: JSONObject,
)

/**
 * ext2 / exFAT / FAT32 を作る手順。流れ: [open] (計画を作る・書かない) → [plan] → (ユーザーが [MkfsPlan.confirmPhrase] を入力) → [apply]。
 * 確認文字列は、既存のファイルシステムやパーティション表を壊す計画なら "ERASE xxxxxx"、何も無い領域なら "FORMAT xxxxxx"。
 * 書き込み可で開いたデバイスに対して適用は同時に 1 つだけ。マウント中は拒否する。必ずデバイスを閉じる**前**に [close] すること。
 */
class MkfsSession private constructor(private var h: Long) : AutoCloseable {
    private fun s(x: String) = x.toByteArray(Charsets.UTF_8)
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    private fun fail(rc: Int, what: String): Nothing {
        val parts = String(DiskTools.nativeMkfsLastError(h), Charsets.UTF_8).split('\n', limit = 2)
        val detail = parts.getOrElse(0) { "" }
        throw DiskToolsException(rc, detail, "$what failed: ${parts.getOrElse(1) { "" }.ifEmpty { NativeBridge.nativeErrcName(rc) }} ($detail)")
    }

    fun plan(): MkfsPlan {
        val e = IntArray(1)
        val b = DiskTools.nativeMkfsPlanJson(live(), e) ?: fail(e[0], "plan")
        return parsePlan(String(b, Charsets.UTF_8))
    }

    /**
     * 計画を適用する。[typedPhrase] はユーザーが入力した文字列 (計画の confirmPhrase と一致しなければ書かずに拒否)。
     * [backupPath] に上書きする領域の元の内容を保存してから書く (保存できなければ何も書かない)。[inUse] = 適用の直前にマウント中か。
     * 失敗しても例外ではなく結果を返す (ok = false、rolledBack で元に戻したかどうかが分かる)。
     */
    fun apply(typedPhrase: String, backupPath: String?, inUse: Boolean = false, job: DiskJob? = null, timestampSec: Long = System.currentTimeMillis() / 1000): MkfsApplyResult {
        val e = IntArray(1)
        val tok = DiskTools.nativeMkfsMakeToken(live(), s(typedPhrase), e) ?: fail(e[0], "token")
        val b = DiskTools.nativeMkfsApply(live(), tok, backupPath?.let(::s), timestampSec, inUse, job?.handle() ?: 0L, e) ?: fail(e[0], "apply")
        val j = JSONObject(String(b, Charsets.UTF_8))
        return MkfsApplyResult(j.getBoolean("ok"), j.getString("code"), j.optString("message"), j.optString("bytesWritten", "0").toLong(),
            j.optBoolean("rolledBack"), j.optBoolean("verified"), j.optString("checkVerdict"))
    }

    /** 直近の apply が書き込む前に作ったバックアップ (apply に backupPath を渡さなかったときの保険) */
    fun lastBackup(): ByteArray? {
        val e = IntArray(1)
        return DiskTools.nativeMkfsLastBackup(live(), e)
    }

    override fun close() {
        if (Handle.valid(h)) DiskTools.nativeMkfsClose(h)
        h = 0
    }

    companion object {
        /**
         * [deviceHandle] の [offset, offset+length) に [fsName] ("ext2" / "exfat" / "fat32") を作る計画を作る。
         * [clusterSize] = 0 は自動 (ext2 ではブロックサイズ)。[length] = 0 はデバイスの終わりまで。[inUse] = マウント中 (計画が不可になる)。
         */
        fun open(deviceHandle: Long, fsName: String, offset: Long = 0, length: Long = 0, label: String? = null, clusterSize: Int = 0, sectorSize: Int = 0,
                 inodeRatio: Int = 0, inodeSize: Int = 0, inUse: Boolean = false): MkfsSession {
            val h = DiskTools.nativeMkfsOpen(deviceHandle, fsName.toByteArray(Charsets.UTF_8), offset, length, label?.toByteArray(Charsets.UTF_8), clusterSize,
                sectorSize, inodeRatio, inodeSize, inUse, 0, 0)
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                val parts = String(DiskTools.nativeMkfsLastError(0), Charsets.UTF_8).split('\n', limit = 2)
                throw DiskToolsException(code, parts.getOrElse(0) { "" }, "open failed: ${NativeBridge.nativeErrcName(code)} (${parts.getOrElse(0) { "" }})")
            }
            return MkfsSession(h)
        }

        /** 作成に必要な最小サイズ (バイト)。0 = 非対応 */
        fun minSize(fsName: String, sectorSize: Int = 512): Long = DiskTools.nativeMkfsMinSize(fsName.toByteArray(Charsets.UTF_8), sectorSize)

        /** バックアップの中身を検査する (version / timestamp / rangeOffset / rangeLength / regions / bytes)。壊れていれば例外 */
        fun backupInfo(data: ByteArray): JSONObject {
            val e = IntArray(1)
            val b = DiskTools.nativeMkfsBackupInfo(data, e) ?: throw DiskToolsException(e[0], "backup-corrupt", "not a valid mkfs backup")
            return JSONObject(String(b, Charsets.UTF_8))
        }

        /** バックアップした領域を書き戻す (書き込み可のデバイス。ゼロ埋めされただけの大きな領域は戻らない)。失敗は例外 */
        fun restore(deviceHandle: Long, backup: ByteArray) {
            val rc = DiskTools.nativeMkfsRestore(deviceHandle, backup)
            if (rc != 0) throw DiskToolsException(-rc, "restore-failed", "restore failed: ${NativeBridge.nativeErrcName(-rc)}")
        }

        private fun strings(a: JSONArray?): List<String> = if (a == null) emptyList() else List(a.length()) { i ->
            val v = a.get(i)
            if (v is JSONObject) v.optString("message", v.toString()) else v.toString()
        }

        private fun parsePlan(json: String): MkfsPlan {
            val o = JSONObject(json)
            val lay = o.optJSONObject("layout")
            val layout = LinkedHashMap<String, String>()
            lay?.keys()?.forEach { k -> layout[k] = lay.optString(k) }
            return MkfsPlan(o.getBoolean("ok"), o.optString("fs"), o.optString("existingFs"), o.optBoolean("requiresForce"), o.optString("confirmPhrase"),
                o.optString("planHash"), o.optString("bytesToWrite", "0").toLong(), strings(o.optJSONArray("changes")), strings(o.optJSONArray("problems")), layout, o)
        }
    }
}
