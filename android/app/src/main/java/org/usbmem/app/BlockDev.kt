package org.usbmem.app

/**
 * ディスクイメージ (QCOW2 / VHD / VHDX / VMDK)・COW オーバーレイ・NBD クライアントの JNI 入口。
 * ハンドルの扱いは [NativeBridge] / [Handle] と同じ (usbmem_device* を jlong にしたもの。閉じるのは NativeBridge.nativeClose)。
 * UI にはまだつないでいない (coordinator が統合する)。
 */
object BlockDevNative {
    init {
        System.loadLibrary("usbmem_jni")
    }

    /** infoOut[0] に JSON (UTF-8) を入れる。失敗時も理由 ("detail") が入る。 */
    @JvmStatic external fun nativeImageOpen(fileHandle: Long, resolver: ImageResolver?, infoOut: Array<ByteArray?>?): Long
    @JvmStatic external fun nativeImageOpenPath(path: ByteArray, allowUnsafe: Boolean, infoOut: Array<ByteArray?>?): Long
    @JvmStatic external fun nativeImageDetect(fileHandle: Long): ByteArray?
    @JvmStatic external fun nativeOverlayCreate(base: Long, logPath: ByteArray?, blockSize: Int, maxBytes: Long): Long
    @JvmStatic external fun nativeOverlayDirtyBytes(handle: Long): Long
    @JvmStatic external fun nativeOverlayCommit(handle: Long, target: Long, clearAfter: Boolean): Int
    @JvmStatic external fun nativeOverlayDiscard(handle: Long): Int
    @JvmStatic external fun nativeNbdOpen(
        host: ByteArray?, port: Int, unixSocket: ByteArray?, exportName: ByteArray?, readOnly: Boolean,
        connectMs: Int, ioMs: Int, detailOut: Array<ByteArray?>?,
    ): Long
    @JvmStatic external fun nativeNbdListExports(host: ByteArray?, port: Int, unixSocket: ByteArray?, errorOut: IntArray?, detailOut: Array<ByteArray?>?): ByteArray?
    @JvmStatic external fun nativeNbdInfo(handle: Long, errorOut: IntArray?): ByteArray?
}

/**
 * イメージが参照する別ファイル (バッキング / 親 / VMDK のエクステント) を開く。detach した読み取り用 fd を返す (失敗は負)。
 * [name] はイメージ内の名前 (UTF-8, '/' 区切り)。**信用しない**: 絶対パスや ".." を含みうる。SAF では兄弟ファイルをユーザーに選ばせる。
 * [kind] 0 = バッキング(親) / 1 = エクステント。[formatHint] は "qcow2" / "raw" / "vmdk" / "vpc" / "vhdx" / ""。
 * JNI から呼ばれるので、メソッド名・シグネチャを変えないこと ("open", (ByteArray, Int, ByteArray) -> Int)。
 */
fun interface ImageResolver {
    fun open(name: ByteArray, kind: Int, formatHint: ByteArray): Int
}

/** 開いたイメージの情報 (JSON をそのまま保持。UI 用の項目は coordinator が決める)。 */
data class ImageInfoJson(val json: String) {
    val format: String get() = Regex("\"format\":\"([a-z0-9]+)\"").find(json)?.groupValues?.get(1) ?: ""
}

/** ネイティブのデバイスハンドルを持つ (close で閉じる。下位のファイルも閉じる)。パーティション解析・FS マウントに渡せる。 */
class BlockDeviceHandle internal constructor(handle: Long, val info: ImageInfoJson?) : AutoCloseable {
    var handle: Long = handle
        private set

    fun partitionTable(): PartitionTable {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(handle, err)
            ?: throw NativeException(err[0], "partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        return PartitionTable.parse(String(bytes, Charsets.UTF_8))
    }

    override fun close() {
        if (Handle.valid(handle)) NativeBridge.nativeClose(handle)
        handle = 0
    }

    companion object {
        private fun fail(h: Long, what: String, detail: String? = null): Nothing {
            val code = (-h).toInt()
            throw NativeException(code, "$what failed: ${NativeBridge.nativeErrcName(code)}" + (if (detail.isNullOrEmpty()) "" else " ($detail)"))
        }

        private fun detailOf(out: Array<ByteArray?>): String? = out[0]?.let { String(it, Charsets.UTF_8) }

        /** [file] (RawImage の fd を NativeBridge.nativeOpenFd で開いたハンドル) を形式自動判別で開く。成功するとファイルの所有権はこのハンドルに移る。 */
        fun openImage(fileHandle: Long, resolver: ImageResolver? = null): BlockDeviceHandle {
            val out = arrayOfNulls<ByteArray>(1)
            val h = BlockDevNative.nativeImageOpen(fileHandle, resolver, out)
            if (Handle.isError(h)) fail(h, "image open", detailOf(out)?.let { Regex("\"detail\":\"([^\"]*)\"").find(it)?.groupValues?.get(1) })
            return BlockDeviceHandle(h, detailOf(out)?.let { ImageInfoJson(it) })
        }

        /** 書き込みを溜める COW オーバーレイ。ベースには書かない。logPath を指定すればファイルに保存して再開できる。 */
        fun overlay(base: BlockDeviceHandle, logPath: String? = null, blockSize: Int = 0, maxBytes: Long = 0): BlockDeviceHandle {
            val h = BlockDevNative.nativeOverlayCreate(base.handle, logPath?.toByteArray(Charsets.UTF_8), blockSize, maxBytes)
            if (Handle.isError(h)) fail(h, "overlay create")
            return BlockDeviceHandle(h, null)
        }

        /** NBD。host 指定でネットワーク越しにつなぐには AndroidManifest の INTERNET 権限が必要 (UNIX ソケットは不要)。 */
        fun nbd(host: String?, port: Int = 10809, unixSocket: String? = null, exportName: String = "", readOnly: Boolean = true,
                connectMs: Int = 0, ioMs: Int = 0): BlockDeviceHandle {
            val detail = arrayOfNulls<ByteArray>(1)
            val h = BlockDevNative.nativeNbdOpen(host?.toByteArray(Charsets.UTF_8), port, unixSocket?.toByteArray(Charsets.UTF_8),
                exportName.toByteArray(Charsets.UTF_8), readOnly, connectMs, ioMs, detail)
            if (Handle.isError(h)) fail(h, "nbd open", detailOf(detail))
            return BlockDeviceHandle(h, null)
        }
    }

    /** オーバーレイの書き込み量 (バイト) */
    fun overlayDirtyBytes(): Long = BlockDevNative.nativeOverlayDirtyBytes(handle)

    /** オーバーレイの内容を [target] (書き込み可能) へ書き戻す。 */
    fun overlayCommit(target: BlockDeviceHandle, clearAfter: Boolean = false) {
        val rc = BlockDevNative.nativeOverlayCommit(handle, target.handle, clearAfter)
        if (rc != 0) throw NativeException(rc, "overlay commit failed: ${NativeBridge.nativeErrcName(rc)}")
    }

    fun overlayDiscard() {
        val rc = BlockDevNative.nativeOverlayDiscard(handle)
        if (rc != 0) throw NativeException(rc, "overlay discard failed: ${NativeBridge.nativeErrcName(rc)}")
    }
}
