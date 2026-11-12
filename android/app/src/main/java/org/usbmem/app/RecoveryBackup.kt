package org.usbmem.app

import org.json.JSONObject

/**
 * データ救出 (ddrescue 方式) とバックアップ / 復元 (Partclone 方式) への入口 (docs/recovery-backup.md)。UI は未接続。
 *
 * どの処理も、呼び出したスレッドをふさぐ。必ず Dispatchers.IO などの別スレッドで呼び、取り消し・進捗は [RecoveryJob] 経由で他のスレッドから行う。
 * デバイスは NativeBridge.nativeOpenFd / nativeOpenUsbMsc / パーティション切り出しで得た **ハンドル** (Long)。
 *  - 救出: 読み取り元 [inHandle] には書き込まない。出力先 [outHandle] は書き込み可で開いたファイル (SAF の fd など) か別のデバイス。
 *  - バックアップ: [srcHandle] は「ファイルシステムの先頭から始まる」デバイス (パーティションなら切り出したもの)。暗号化されていれば先に復号する。
 *  - 復元: 書き込み先を上書きする。plan → ユーザーが確認文字列を打つ → token → apply の順を必ず通す。
 */
object RecoveryNative {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeJobNew(): Long
    @JvmStatic external fun nativeJobCancel(job: Long)
    @JvmStatic external fun nativeJobPause(job: Long, paused: Boolean)
    @JvmStatic external fun nativeJobProgress(job: Long): ByteArray?
    @JvmStatic external fun nativeJobFree(job: Long)
    @JvmStatic external fun nativeRescueRun(inHandle: Long, outHandle: Long, mapPath: String?, opts: LongArray?, job: Long, resultOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativeBackupRun(srcHandle: Long, imagePath: String, lz4: Boolean, fullIfUnclean: Boolean, job: Long, resultOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativeInspect(imagePath: String, verify: Boolean, resultOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativeRestorePlan(imagePath: String, target: Long, inUse: Boolean, verifyAll: Boolean, planOut: LongArray, jsonOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativeRestoreToken(plan: Long, typed: String, tokenOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativeRestoreApply(plan: Long, imagePath: String, target: Long, token: String, inUse: Boolean, job: Long, resultOut: Array<ByteArray?>?): Int
    @JvmStatic external fun nativePlanFree(plan: Long)
}

/** 実行中の仕事。別スレッドから取り消し・一時停止・進捗の取得ができる。使い終わったら close()。 */
class RecoveryJob : AutoCloseable {
    internal val handle: Long = RecoveryNative.nativeJobNew()
    private var closed = false

    fun cancel() { if (!closed) RecoveryNative.nativeJobCancel(handle) }
    /** 救出だけ。true で一時停止、false で再開。 */
    fun pause(paused: Boolean) { if (!closed) RecoveryNative.nativeJobPause(handle, paused) }
    /** 直近の進捗 (phase / pos / size / finished / bad / readErrors / done / total ...)。まだなければ null。 */
    fun progress(): JSONObject? = if (closed) null else RecoveryNative.nativeJobProgress(handle)?.let { JSONObject(String(it, Charsets.UTF_8)) }
    override fun close() { if (!closed) { closed = true; RecoveryNative.nativeJobFree(handle) } }
}

/** 救出の設定。安全弁 (0 = 無効) は USB デバイスを痛めないための自動停止。 */
data class RescueOptions(
    val start: Long = 0, val length: Long = 0, val outOffset: Long = 0,
    val sectorSize: Int = 0, val clusterSectors: Int = 64, val retries: Int = 0,
    val noTrim: Boolean = false, val noScrape: Boolean = false, val noSkip: Boolean = false,
    val readTimeoutMs: Int = 0,
    val maxConsecutiveErrors: Int = 0, val maxErrors: Long = 0,
    val errorWindowMs: Int = 0, val maxErrorsInWindow: Int = 0,
    val noProgressTimeoutMs: Long = 0, val pauseBetweenReadsMs: Int = 0,
) {
    internal fun toArray() = longArrayOf(
        start, length, outOffset, sectorSize.toLong(), clusterSectors.toLong(), retries.toLong(),
        if (noTrim) 1 else 0, if (noScrape) 1 else 0, if (noSkip) 1 else 0, readTimeoutMs.toLong(),
        maxConsecutiveErrors.toLong(), maxErrors, errorWindowMs.toLong(), maxErrorsInWindow.toLong(), noProgressTimeoutMs, pauseBetweenReadsMs.toLong(), 0L,
    )
}

/** 失敗の詳細。code は機械可読の理由 ("fs-not-clean" "target-in-use" "bad-token" ...)、errc は usbmem::Errc。 */
class RecoveryException(val errc: Int, val code: String, message: String) : Exception(message)

private fun parse(rc: Int, out: Array<ByteArray?>): JSONObject {
    val j = out[0]?.let { JSONObject(String(it, Charsets.UTF_8)) }
    if (j == null) throw RecoveryException(rc, "native-error", "native error ${NativeBridge.nativeErrcName(rc)}")
    return j
}

object Rescue {
    /**
     * 読み取り元 [inHandle] から出力先 [outHandle] へ複製する。[mapPath] があれば続きから行い、進捗を保存し続ける (壊れていれば断る)。
     * 結果 JSON の "stop": finished / cancelled / safety-abort / input-gone / output-error / mapfile-error / bad-argument。
     * cancelled・safety-abort・input-gone は、同じ mapfile で再実行すれば続きから行える。
     */
    fun run(inHandle: Long, outHandle: Long, mapPath: String?, options: RescueOptions = RescueOptions(), job: RecoveryJob? = null): JSONObject {
        val out = arrayOfNulls<ByteArray>(1)
        val rc = RecoveryNative.nativeRescueRun(inHandle, outHandle, mapPath, options.toArray(), job?.handle ?: 0L, out)
        return parse(rc, out)
    }
}

object Backup {
    /** [srcHandle] の使用中ブロックだけを [imagePath] に Partclone 形式で書く。失敗は結果 JSON の ok=false (code / message)。lz4 で圧縮。 */
    fun run(srcHandle: Long, imagePath: String, lz4: Boolean = true, fullIfUnclean: Boolean = false, job: RecoveryJob? = null): JSONObject {
        val out = arrayOfNulls<ByteArray>(1)
        val rc = RecoveryNative.nativeBackupRun(srcHandle, imagePath, lz4, fullIfUnclean, job?.handle ?: 0L, out)
        return parse(rc, out)
    }

    /** 画像の中身を調べる (verify = true なら全体のチェックサムも確かめる)。lz4 は自動判別。 */
    fun inspect(imagePath: String, verify: Boolean = false): JSONObject {
        val out = arrayOfNulls<ByteArray>(1)
        val rc = RecoveryNative.nativeInspect(imagePath, verify, out)
        return parse(rc, out)
    }
}

/** 復元の計画。ok でなくても作られる (理由を見せるため)。apply は ok の計画だけ受け付ける。close() で解放。 */
class RestorePlan private constructor(private val handle: Long, val json: JSONObject, private val imagePath: String) : AutoCloseable {
    private var closed = false
    val ok: Boolean get() = json.optBoolean("ok")
    val code: String get() = json.optString("code")
    val message: String get() = json.optString("message")
    /** ユーザーに打ち込ませる文字列 ("RESTORE xxxxxx")。 */
    val confirmPhrase: String get() = json.optString("confirmPhrase")
    val warnings: List<String> get() = json.optJSONArray("warnings")?.let { a -> List(a.length()) { a.getString(it) } } ?: emptyList()

    /** ユーザーが打った文字列から確認トークンを作る。confirmPhrase と一致していなければ apply が断る。 */
    fun token(typed: String): String {
        check(!closed) { "closed" }
        val out = arrayOfNulls<ByteArray>(1)
        val rc = RecoveryNative.nativeRestoreToken(handle, typed, out)
        val t = out[0] ?: throw RecoveryException(rc, "native-error", "could not make a token")
        return String(t, Charsets.UTF_8)
    }

    /** 適用する。書き込み先は計画のときと同じデバイス。結果 JSON: ok / code / message / blocksWritten / verified / targetTouched。 */
    fun apply(targetHandle: Long, token: String, targetInUse: Boolean = false, job: RecoveryJob? = null): JSONObject {
        check(!closed) { "closed" }
        val out = arrayOfNulls<ByteArray>(1)
        val rc = RecoveryNative.nativeRestoreApply(handle, imagePath, targetHandle, token, targetInUse, job?.handle ?: 0L, out)
        return parse(rc, out)
    }

    override fun close() { if (!closed) { closed = true; if (handle != 0L) RecoveryNative.nativePlanFree(handle) } }

    companion object {
        /** 画像と書き込み先から計画を作る (何も書かない)。verifyAll = true なら画像全体を先に検査する。 */
        fun create(imagePath: String, targetHandle: Long, targetInUse: Boolean = false, verifyAll: Boolean = true): RestorePlan {
            val h = LongArray(1)
            val out = arrayOfNulls<ByteArray>(1)
            val rc = RecoveryNative.nativeRestorePlan(imagePath, targetHandle, targetInUse, verifyAll, h, out)
            return RestorePlan(h[0], parse(rc, out), imagePath)
        }
    }
}
