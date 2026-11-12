package org.usbmem.app

import org.json.JSONArray
import org.json.JSONObject

/**
 * パーティション編集のネイティブ入口 (jni_partedit.cpp)。NativeBridge とは別の object。UI は未接続 (core の API と JNI まで)。
 * 使うときは [PartitionEditSession] 経由にする。設計と安全モデルは docs/partition-edit.md。
 *
 * 戻り値の約束は NativeBridge と同じ: 0 以外の Int は -(usbmem::Errc)、byte[] が null のときは errorOut[0] にコード。
 */
object PartitionEditor {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeOpen(devHandle: Long, sectorSize: Int, inUse: Boolean, rngSeed: Long): Long
    @JvmStatic external fun nativeClose(h: Long)
    @JvmStatic external fun nativeSetInUse(h: Long, inUse: Boolean)
    @JvmStatic external fun nativeLastError(h: Long): ByteArray
    @JvmStatic external fun nativeTableJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeGptJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeCreate(h: Long, role: Int, start: Long, count: Long, mbrType: Int, typeGuid: ByteArray?, uniqueGuid: ByteArray?,
                                         name: ByteArray?, attrs: Long, bootable: Boolean, outHandle: IntArray): Int
    @JvmStatic external fun nativeRemove(h: Long, part: Int): Int
    @JvmStatic external fun nativeResize(h: Long, part: Int, count: Long): Int
    @JvmStatic external fun nativeMove(h: Long, part: Int, start: Long): Int
    @JvmStatic external fun nativeSetMbrType(h: Long, part: Int, type: Int): Int
    @JvmStatic external fun nativeSetGptType(h: Long, part: Int, guid: ByteArray): Int
    @JvmStatic external fun nativeSetName(h: Long, part: Int, name: ByteArray): Int
    @JvmStatic external fun nativeSetAttrs(h: Long, part: Int, attrs: Long): Int
    @JvmStatic external fun nativeSetBootable(h: Long, part: Int, bootable: Boolean): Int
    @JvmStatic external fun nativeSetUniqueGuid(h: Long, part: Int, guid: ByteArray): Int
    @JvmStatic external fun nativeSetDiskId(h: Long, id: ByteArray): Int
    @JvmStatic external fun nativeNewTable(h: Long, kind: Int, entries: Int, overwriteFs: Boolean): Int
    @JvmStatic external fun nativeRepairGpt(h: Long, kind: Int): Int
    @JvmStatic external fun nativeClearOps(h: Long)
    @JvmStatic external fun nativePlanJson(h: Long, includeSectors: Boolean, err: IntArray): ByteArray?
    @JvmStatic external fun nativeMakeToken(h: Long, typed: ByteArray, err: IntArray): ByteArray?
    /** 失敗しても結果 JSON (code / message / rolledBack ...) が返る。err[0] は戻り値 (0 = 成功)。null は引数不正など */
    @JvmStatic external fun nativeApply(h: Long, token: ByteArray, backupPath: ByteArray?, timestamp: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBackup(h: Long, timestamp: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeLastBackup(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeBackupInfo(data: ByteArray, err: IntArray): ByteArray?
    @JvmStatic external fun nativePlanRestore(h: Long, data: ByteArray, err: IntArray): ByteArray?
    @JvmStatic external fun nativeExportSfdisk(h: Long, deviceName: ByteArray, err: IntArray): ByteArray?
}

/** 編集操作の失敗。[detail] は機械可読なコード ("busy" / "overlap" / "bad-token" / "device-read-only" / "in-use" ...) */
class PartitionEditException(val code: Int, val detail: String, message: String) : Exception(message)

/** 適用結果 (apply の JSON)。[rolledBack] = 書き込み中の失敗を元に戻した、[verified] = 書いた後に読み戻して一致を確認した */
data class PartitionApplyResult(val ok: Boolean, val code: String, val message: String, val sectorsWritten: Int, val rolledBack: Boolean, val verified: Boolean)

/** 計画 (UI が差分と確認文字列を見せるためのもの)。[json] は全体 (stages / before / after を含む) */
data class PartitionPlan(
    val ok: Boolean,
    val destructive: Boolean,
    val requiresForce: Boolean,
    val confirmPhrase: String,
    val planHash: String,
    val sectorCount: Int,
    val changes: List<String>,
    val problems: List<String>,
    val json: JSONObject,
)

/**
 * 1 つのデバイスに対する編集セッション。流れ: 操作 → [plan] → (ユーザーが [PartitionPlan.confirmPhrase] を入力) → [apply]。
 * 操作を足すと計画は無効になる。書き込み可で開いたデバイスに対するセッションは同時に 1 つだけ (2 つ目の open は "busy")。
 * 必ずデバイス (RawImage 等) を閉じる**前**に [close] すること。
 */
class PartitionEditSession private constructor(private var h: Long) : AutoCloseable {
    private fun s(x: String) = x.toByteArray(Charsets.UTF_8)
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    private fun fail(rc: Int, what: String): Nothing {
        val parts = String(PartitionEditor.nativeLastError(h), Charsets.UTF_8).split('\n', limit = 2)
        val detail = parts.getOrElse(0) { "" }
        throw PartitionEditException(rc, detail, "$what failed: ${parts.getOrElse(1) { "" }.ifEmpty { NativeBridge.nativeErrcName(rc) }} ($detail)")
    }

    private fun op(what: String, rc: Int) { if (rc != 0) fail(-rc, what) }

    private fun text(what: String, call: (IntArray) -> ByteArray?): String {
        val err = IntArray(1)
        val b = call(err) ?: fail(err[0], what)
        return String(b, Charsets.UTF_8)
    }

    /** ディスク上の現在のテーブル (open 時点。編集の影響は受けない)。キーは PartitionTable の JSON と同じ */
    fun tableJson(): JSONObject = JSONObject(text("table") { PartitionEditor.nativeTableJson(live(), it) })

    /** GPT の詳細検査 (primary / backup の状態、診断、suggestedRepairs) */
    fun gptInspection(): JSONObject = JSONObject(text("inspect") { PartitionEditor.nativeGptJson(live(), it) })

    /** 新しいパーティションを作り、そのハンドルを返す。start / count = 0 は自動配置 / 空きの最後まで。role: 0 自動 1 主 2 拡張 3 論理 (MBR) */
    fun create(role: Int = 0, start: Long = 0, count: Long = 0, mbrType: Int = 0x83, typeGuid: String? = null, uniqueGuid: String? = null,
               name: String? = null, attrs: Long = 0, bootable: Boolean = false): Int {
        val out = IntArray(1)
        op("create", PartitionEditor.nativeCreate(live(), role, start, count, mbrType, typeGuid?.let(::s), uniqueGuid?.let(::s), name?.let(::s), attrs, bootable, out))
        return out[0]
    }
    fun remove(part: Int) = op("remove", PartitionEditor.nativeRemove(live(), part))
    fun resize(part: Int, count: Long) = op("resize", PartitionEditor.nativeResize(live(), part, count))
    fun move(part: Int, start: Long) = op("move", PartitionEditor.nativeMove(live(), part, start))
    fun setMbrType(part: Int, type: Int) = op("set type", PartitionEditor.nativeSetMbrType(live(), part, type))
    fun setGptType(part: Int, guid: String) = op("set type", PartitionEditor.nativeSetGptType(live(), part, s(guid)))
    fun setName(part: Int, name: String) = op("set name", PartitionEditor.nativeSetName(live(), part, s(name)))
    fun setAttrs(part: Int, attrs: Long) = op("set attributes", PartitionEditor.nativeSetAttrs(live(), part, attrs))
    fun setBootable(part: Int, bootable: Boolean) = op("set bootable", PartitionEditor.nativeSetBootable(live(), part, bootable))
    fun setUniqueGuid(part: Int, guid: String) = op("set GUID", PartitionEditor.nativeSetUniqueGuid(live(), part, s(guid)))
    /** MBR: 0x12345678 形式 / GPT: GUID */
    fun setDiskId(id: String) = op("set disk id", PartitionEditor.nativeSetDiskId(live(), s(id)))
    /** 新しいテーブルを作る (既存のパーティションは全部消える)。パーティション無しのファイルシステムの上に作るなら overwriteFilesystem = true を明示 */
    fun newTable(gpt: Boolean, gptEntries: Int = 128, overwriteFilesystem: Boolean = false) =
        op("new table", PartitionEditor.nativeNewTable(live(), if (gpt) 2 else 1, gptEntries, overwriteFilesystem))
    /** GPT の修復を最初の操作として積む */
    fun repairGpt(kind: GptRepair) = op("repair", PartitionEditor.nativeRepairGpt(live(), kind.code))
    fun clearOps() = PartitionEditor.nativeClearOps(live())
    fun setInUse(inUse: Boolean) = PartitionEditor.nativeSetInUse(live(), inUse)

    /** 検証・差分・書き込み予定を作る (書き込みはしない)。問題があれば ok = false で problems に理由が入る */
    fun plan(includeSectors: Boolean = false): PartitionPlan = parsePlan(text("plan") { PartitionEditor.nativePlanJson(live(), includeSectors, it) })

    /** バックアップファイルから復元する計画を作る (不一致があれば requiresForce / FORCE 確認文字列になる) */
    fun planRestore(backup: ByteArray): PartitionPlan = parsePlan(text("plan restore") { PartitionEditor.nativePlanRestore(live(), backup, it) })

    /**
     * 直近の計画を適用する。[typedPhrase] はユーザーが入力した文字列 (計画の confirmPhrase と一致しなければ書かずに拒否)。
     * [backupPath] に書き込み前の表を保存してから書く (保存できなければ何も書かない)。失敗しても例外ではなく結果を返す
     * (ok = false、rolledBack で元に戻したかどうかが分かる)。
     */
    fun apply(typedPhrase: String, backupPath: String?, timestampSec: Long = System.currentTimeMillis() / 1000): PartitionApplyResult {
        val e = IntArray(1)
        val tok = PartitionEditor.nativeMakeToken(live(), s(typedPhrase), e) ?: fail(e[0], "token")
        val b = PartitionEditor.nativeApply(live(), tok, backupPath?.let(::s), timestampSec, e) ?: fail(e[0], "apply")
        val j = JSONObject(String(b, Charsets.UTF_8))
        return PartitionApplyResult(j.getBoolean("ok"), j.getString("code"), j.optString("message"), j.optInt("sectorsWritten"),
            j.optBoolean("rolledBack"), j.optBoolean("verified"))
    }

    /** 現在のディスク上のテーブルを 1 ファイル分のバイト列にする (保存は呼び出し側) */
    fun backup(timestampSec: Long = System.currentTimeMillis() / 1000): ByteArray {
        val e = IntArray(1)
        return PartitionEditor.nativeBackup(live(), timestampSec, e) ?: fail(e[0], "backup")
    }
    /** 直近の apply が書き込む前に作った表のバックアップ (apply に backupPath を渡さなかったときの保険) */
    fun lastBackup(): ByteArray? {
        val e = IntArray(1)
        return PartitionEditor.nativeLastBackup(live(), e)
    }
    /** sfdisk --dump 互換のテキスト (現在のディスク上のテーブル) */
    fun exportSfdisk(deviceName: String = "/dev/disk"): String = text("export") { PartitionEditor.nativeExportSfdisk(live(), s(deviceName), it) }

    override fun close() {
        if (Handle.valid(h)) PartitionEditor.nativeClose(h)
        h = 0
    }

    enum class GptRepair(val code: Int) { CopyPrimaryToBackup(1), CopyBackupToPrimary(2), MoveBackupToEnd(3), RebuildProtectiveMbr(4) }

    companion object {
        /** [deviceHandle] は NativeBridge が返したデバイスのハンドル。書き込みたいなら書き込み可で開いたデバイスを渡す */
        fun open(deviceHandle: Long, inUse: Boolean = false, sectorSize: Int = 0): PartitionEditSession {
            val h = PartitionEditor.nativeOpen(deviceHandle, sectorSize, inUse, 0)
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                val parts = String(PartitionEditor.nativeLastError(0), Charsets.UTF_8).split('\n', limit = 2)
                throw PartitionEditException(code, parts.getOrElse(0) { "" }, "open failed: ${NativeBridge.nativeErrcName(code)} (${parts.getOrElse(0) { "" }})")
            }
            return PartitionEditSession(h)
        }

        /** バックアップファイルの中身を検査する (version / sectorSize / tableKind / diskBytes / diskId ...)。壊れていれば例外 */
        fun backupInfo(data: ByteArray): JSONObject {
            val e = IntArray(1)
            val b = PartitionEditor.nativeBackupInfo(data, e) ?: throw PartitionEditException(e[0], "backup-corrupt", "not a valid table backup")
            return JSONObject(String(b, Charsets.UTF_8))
        }

        private fun strings(a: JSONArray?): List<String> = if (a == null) emptyList() else List(a.length()) { i ->
            val v = a.get(i)
            if (v is JSONObject) v.optString("message", v.toString()) else v.toString()
        }

        private fun parsePlan(json: String): PartitionPlan {
            val o = JSONObject(json)
            return PartitionPlan(o.getBoolean("ok"), o.optBoolean("destructive"), o.optBoolean("requiresForce"), o.optString("confirmPhrase"),
                o.optString("planHash"), o.optInt("sectorCount"), strings(o.optJSONArray("changes")), strings(o.optJSONArray("problems")), o)
        }
    }
}
