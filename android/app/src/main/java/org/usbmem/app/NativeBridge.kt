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
