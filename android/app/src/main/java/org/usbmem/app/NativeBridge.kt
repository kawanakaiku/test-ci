package org.usbmem.app

/** ネイティブ層への唯一の入口。Android 固有処理(SAF/USB)はこの外側(Kotlin)に置く。 */
object NativeBridge {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeOpenFd(fd: Int, readOnly: Boolean): Long
    @JvmStatic external fun nativeListPartitions(handle: Long, errorOut: IntArray?): ByteArray?
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeErrcName(code: Int): String
    @JvmStatic external fun nativeNtfsMount(fd: Int): Long
    @JvmStatic external fun nativeNtfsUnmount(handle: Long)
    @JvmStatic external fun nativeNtfsList(handle: Long, path: ByteArray, errorOut: IntArray): ByteArray?
    @JvmStatic external fun nativeNtfsRead(handle: Long, path: ByteArray, offset: Long, buf: ByteArray): Long
}

class NativeException(val code: Int, message: String) : Exception(message)

/** fd の所有権を引き受ける(close でネイティブ側が閉じる)。 */
class RawImage private constructor(private var handle: Long) : AutoCloseable {
    fun partitionTable(): PartitionTable {
        check(handle > 0) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(handle, err)
            ?: throw NativeException(err[0], "partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        return PartitionTable.parse(String(bytes, Charsets.UTF_8))
    }

    override fun close() {
        if (handle > 0) NativeBridge.nativeClose(handle)
        handle = 0
    }

    companion object {
        /** 常に読み取り専用で開く(Phase 1)。 */
        fun openReadOnly(detachedFd: Int): RawImage {
            val h = NativeBridge.nativeOpenFd(detachedFd, true)
            if (h <= 0) {
                val code = (-h).toInt()
                throw NativeException(code, "open failed: ${NativeBridge.nativeErrcName(code)}")
            }
            return RawImage(h)
        }
    }
}

data class NtfsEntry(val name: String, val ino: Long, val isDir: Boolean, val size: Long, val mtimeSec: Long)

/** NTFS を読み取り専用でマウントしたボリューム。fd の所有権を引き受ける。 */
class NtfsVolume private constructor(private var handle: Long) : AutoCloseable {
    @Synchronized
    fun list(path: String): List<NtfsEntry> {
        check(handle > 0) { "closed" }
        val err = IntArray(1)
        val b = NativeBridge.nativeNtfsList(handle, path.toByteArray(Charsets.UTF_8), err)
            ?: throw NativeException(err[0], "list '$path' failed: errno ${err[0]}")
        val arr = org.json.JSONArray(String(b, Charsets.UTF_8))
        return (0 until arr.length()).map {
            val o = arr.getJSONObject(it)
            NtfsEntry(o.getString("name"), o.getString("ino").toLong(), o.getBoolean("dir"), o.getString("size").toLong(), o.getLong("mtime"))
        }
    }

    /** off から buf.size バイトまで読む。EOF で 0。 */
    @Synchronized
    fun read(path: String, offset: Long, buf: ByteArray): Int {
        check(handle > 0) { "closed" }
        val r = NativeBridge.nativeNtfsRead(handle, path.toByteArray(Charsets.UTF_8), offset, buf)
        if (r < 0) throw NativeException((-r).toInt(), "read '$path' failed: errno ${-r}")
        return r.toInt()
    }

    @Synchronized
    override fun close() {
        if (handle > 0) NativeBridge.nativeNtfsUnmount(handle)
        handle = 0
    }

    companion object {
        fun mountReadOnly(detachedFd: Int): NtfsVolume {
            val h = NativeBridge.nativeNtfsMount(detachedFd)
            if (h <= 0) throw NativeException((-h).toInt(), "ntfs mount failed: errno ${-h}")
            return NtfsVolume(h)
        }
    }
}
