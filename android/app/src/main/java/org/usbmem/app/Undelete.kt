package org.usbmem.app

import org.json.JSONArray
import org.json.JSONObject

/**
 * 削除ファイルの一覧と救出 / カービング (PhotoRec 相当) のネイティブ入口 (jni_undelete.cpp)。UI は未接続 (core の API と JNI まで)。
 * 使うときは [UndeleteScanner] と [Carver] (と進捗・取り消しの [UndeleteJob]) 経由にする。設計と限界は docs/undelete-carving.md。
 *
 * 戻り値の約束は NativeBridge / PartitionScan と同じ: 0 以外の Int は -(usbmem::Errc)、byte[] が null のときは err[0] にコード。
 * どちらも**読み取り専用**: 検査するデバイスには書かない (読み取り専用で開いたデバイスを渡してよい)。救出したファイルは別の保存先 (SAF の fd / パス) へ書き出す。
 * 保存先が検査しているメディアと同じにならないようにすること (呼び出し側の責任)。
 */
object Undelete {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeJobNew(): Long
    @JvmStatic external fun nativeJobFree(job: Long)
    @JvmStatic external fun nativeJobCancel(job: Long)
    /** 戻り値: 段階名 (UTF-8)。progress[0] = done, progress[1] = total */
    @JvmStatic external fun nativeJobProgress(job: Long, progress: LongArray): ByteArray

    /** 戻り値: ハンドル (>4095) / -(Errc)。キャンセルされても途中までの結果のハンドルが返り、outRc[0] = 5 (Cancelled) */
    @JvmStatic external fun nativeUndeleteScan(dev: Long, includeEmpty: Boolean, job: Long, outRc: IntArray): Long
    @JvmStatic external fun nativeUndeleteClose(h: Long)
    @JvmStatic external fun nativeUndeleteResultJson(h: Long, err: IntArray): ByteArray?
    /** 失敗・取り消しでも結果 JSON ({"ok":false,"code":..}) が返る。err[0] は戻り値 (0 = 成功) */
    @JvmStatic external fun nativeUndeleteExtractFd(h: Long, id: Int, fd: Int, job: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeUndeleteExtractPath(h: Long, id: Int, path: ByteArray, job: Long, err: IntArray): ByteArray?

    @JvmStatic external fun nativeCarveTypesJson(err: IntArray): ByteArray?
    /** 戻り値: ハンドル (>4095) / -(Errc)。キャンセルされても途中までの結果のハンドルが返り、outRc[0] = 5 (statePath から再開できる) */
    @JvmStatic external fun nativeCarveRun(
        dev: Long, types: ByteArray?, minSize: Long, maxSize: Long, align: Int, unallocatedOnly: Boolean, maxHits: Long,
        statePath: ByteArray?, outDir: ByteArray?, job: Long, outRc: IntArray
    ): Long
    @JvmStatic external fun nativeCarveClose(h: Long)
    @JvmStatic external fun nativeCarveResultJson(h: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeCarveExtractFd(h: Long, index: Int, fd: Int, job: Long, err: IntArray): ByteArray?
    @JvmStatic external fun nativeCarveExtractPath(h: Long, index: Int, path: ByteArray, job: Long, err: IntArray): ByteArray?
}

class UndeleteException(val code: Int, val detail: String, message: String) : Exception(message)

/** 救出 1 件の結果。[ok] = false なら [code] に理由 (cancelled / io-error ...)。[sha256] は書き出したバイト列のもの */
data class ExtractOutcome(val ok: Boolean, val code: String, val bytes: Long, val sha256: String)

private fun parseOutcome(b: ByteArray?, err: IntArray, what: String): ExtractOutcome {
    if (b == null) throw UndeleteException(err[0], what, "$what failed: ${NativeBridge.nativeErrcName(err[0])}")
    val j = JSONObject(String(b, Charsets.UTF_8))
    return ExtractOutcome(j.optBoolean("ok"), j.optString("code"), j.optLong("bytes"), j.optString("sha256"))
}

/** 進捗とキャンセル。処理が終わってから [close]。[cancel] と [progress] は別スレッドから呼んでよい */
class UndeleteJob : AutoCloseable {
    private var h: Long = Undelete.nativeJobNew()
    data class Progress(val phase: String, val done: Long, val total: Long)

    internal fun handle(): Long = h
    fun cancel() { if (Handle.valid(h)) Undelete.nativeJobCancel(h) }
    fun progress(): Progress {
        if (!Handle.valid(h)) return Progress("", 0, 0)
        val p = LongArray(2)
        val phase = String(Undelete.nativeJobProgress(h, p), Charsets.UTF_8)
        return Progress(phase, p[0], p[1])
    }
    override fun close() {
        if (Handle.valid(h)) Undelete.nativeJobFree(h)
        h = 0
    }
}

/**
 * 削除ファイルの一覧。[resultJson] の files[] は name / path / size / mtime / level ("high" "medium" "low" "none") / confidence (0-100) / reusedBlocks /
 * signatureCheck / notes など。**level が "low" や "none" のものを「復元できる」と見せないこと** (上書きされている・構造が壊れている可能性が高い)。
 * 対応: FAT12/16/32・exFAT・ext2/3/4・NTFS。必ずデバイスを閉じる**前**に [close] すること。
 */
class UndeleteScanner private constructor(private var h: Long, val cancelled: Boolean) : AutoCloseable {
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    fun resultJson(): JSONObject {
        val err = IntArray(1)
        val b = Undelete.nativeUndeleteResultJson(live(), err) ?: throw UndeleteException(err[0], "result", "result failed: ${NativeBridge.nativeErrcName(err[0])}")
        return JSONObject(String(b, Charsets.UTF_8))
    }

    /** files[id] を [fd] へ書き出す (fd は閉じない。SAF の ParcelFileDescriptor.detachFd の値など)。失敗・取り消しは例外ではなく ok = false で返る */
    fun extractToFd(id: Int, fd: Int, job: UndeleteJob? = null): ExtractOutcome {
        val err = IntArray(1)
        return parseOutcome(Undelete.nativeUndeleteExtractFd(live(), id, fd, job?.handle() ?: 0L, err), err, "extract")
    }

    /** files[id] をパスへ (新規作成。失敗・取り消しなら作りかけを消す)。保存先は検査しているメディアの外にすること */
    fun extractToPath(id: Int, path: String, job: UndeleteJob? = null): ExtractOutcome {
        val err = IntArray(1)
        return parseOutcome(Undelete.nativeUndeleteExtractPath(live(), id, path.toByteArray(Charsets.UTF_8), job?.handle() ?: 0L, err), err, "extract")
    }

    override fun close() {
        if (Handle.valid(h)) Undelete.nativeUndeleteClose(h)
        h = 0
    }

    companion object {
        /**
         * FS の先頭から始まるデバイス ([deviceHandle]。パーティションなら切り出したもの) を走査する (読み取り専用)。キャンセルされると cancelled = true で途中までの結果を返す。
         * 未対応の FS は例外 (Errc::Unsupported)。
         */
        fun scan(deviceHandle: Long, includeEmpty: Boolean = false, job: UndeleteJob? = null): UndeleteScanner {
            val rc = IntArray(1)
            val h = Undelete.nativeUndeleteScan(deviceHandle, includeEmpty, job?.handle() ?: 0L, rc)
            if (Handle.isError(h)) throw UndeleteException((-h).toInt(), "scan", "scan failed: ${NativeBridge.nativeErrcName((-h).toInt())}")
            return UndeleteScanner(h, rc[0] != 0)
        }
    }
}

/** カービングの設定。[types] = null で全形式 (id は [Carver.types])。[align] 0 = 512。[maxSize] / [maxHits] 0 = 既定 */
data class CarveConfig(
    val types: List<String>? = null,
    val minSize: Long = 0,
    val maxSize: Long = 0,
    val align: Int = 0,
    val unallocatedOnly: Boolean = false,
    val maxHits: Long = 0,
)

/**
 * カービング (署名と構造から全体を走査してファイルを切り出す)。断片化したファイルは扱わない。
 * [stateFile] を渡すと進捗を保存し、キャンセル・中断の後で同じ設定・同じデバイスなら続きから再開する。[outDir] を渡すと見つけ次第そこへ書き出す
 * (名前は resultJson の hits[].name)。必ずデバイスを閉じる**前**に [close] すること。
 */
class Carver private constructor(private var h: Long, val cancelled: Boolean) : AutoCloseable {
    private fun live(): Long { check(Handle.valid(h)) { "closed" }; return h }

    /** {"fs","allocationKnown","finished","pos","devSize","bytesScanned","rejected","warnings":[..],"hits":[{"offset","length","format","ext","detail","confidence","name"}]} */
    fun resultJson(): JSONObject {
        val err = IntArray(1)
        val b = Undelete.nativeCarveResultJson(live(), err) ?: throw UndeleteException(err[0], "result", "result failed: ${NativeBridge.nativeErrcName(err[0])}")
        return JSONObject(String(b, Charsets.UTF_8))
    }

    fun extractToFd(index: Int, fd: Int, job: UndeleteJob? = null): ExtractOutcome {
        val err = IntArray(1)
        return parseOutcome(Undelete.nativeCarveExtractFd(live(), index, fd, job?.handle() ?: 0L, err), err, "extract")
    }

    fun extractToPath(index: Int, path: String, job: UndeleteJob? = null): ExtractOutcome {
        val err = IntArray(1)
        return parseOutcome(Undelete.nativeCarveExtractPath(live(), index, path.toByteArray(Charsets.UTF_8), job?.handle() ?: 0L, err), err, "extract")
    }

    override fun close() {
        if (Handle.valid(h)) Undelete.nativeCarveClose(h)
        h = 0
    }

    companion object {
        /** 対応する形式 ([{"id","extensions","description"}]) */
        fun types(): JSONArray {
            val err = IntArray(1)
            val b = Undelete.nativeCarveTypesJson(err) ?: throw UndeleteException(err[0], "types", "types failed: ${NativeBridge.nativeErrcName(err[0])}")
            return JSONArray(String(b, Charsets.UTF_8))
        }

        /**
         * [deviceHandle] を走査する (読み取り専用)。キャンセルされると cancelled = true で途中までの結果を返す (stateFile があれば再開できる)。
         * 別の設定・別のデバイスの stateFile は InvalidArgument、壊れていれば Corrupt の例外。
         */
        fun run(deviceHandle: Long, config: CarveConfig = CarveConfig(), stateFile: String? = null, outDir: String? = null, job: UndeleteJob? = null): Carver {
            val rc = IntArray(1)
            val h = Undelete.nativeCarveRun(
                deviceHandle, config.types?.joinToString(",")?.toByteArray(Charsets.UTF_8), config.minSize, config.maxSize, config.align, config.unallocatedOnly, config.maxHits,
                stateFile?.toByteArray(Charsets.UTF_8), outDir?.toByteArray(Charsets.UTF_8), job?.handle() ?: 0L, rc
            )
            if (Handle.isError(h)) throw UndeleteException((-h).toInt(), "carve", "carve failed: ${NativeBridge.nativeErrcName((-h).toInt())}")
            return Carver(h, rc[0] != 0)
        }
    }
}
