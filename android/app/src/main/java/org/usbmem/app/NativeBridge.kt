package org.usbmem.app

/** ネイティブ層への唯一の入口。Android 固有処理(SAF/USB)はこの外側(Kotlin)に置く。 */
object NativeBridge {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeOpenFd(fd: Int, readOnly: Boolean): Long
    @JvmStatic external fun nativeListPartitions(handle: Long, errorOut: IntArray?): ByteArray?
    @JvmStatic external fun nativeOpenUsbMsc(fd: Int, epIn: Int, epOut: Int, iface: Int, lun: Int, readOnly: Boolean): Long
    @JvmStatic external fun nativeUsbLastSense(handle: Long, out: IntArray)
    @JvmStatic external fun nativeNtfsMountDevice(devHandle: Long, offset: Long, length: Long): Long
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeErrcName(code: Int): String
    @JvmStatic external fun nativeNtfsMount(fd: Int): Long
    @JvmStatic external fun nativeNtfsUnmount(handle: Long)
    @JvmStatic external fun nativeNtfsList(handle: Long, path: ByteArray, errorOut: IntArray): ByteArray?
    @JvmStatic external fun nativeNtfsStat(handle: Long, path: ByteArray, errorOut: IntArray): ByteArray?
    @JvmStatic external fun nativeNtfsRead(handle: Long, path: ByteArray, offset: Long, buf: ByteArray): Long
    // --- 書き込み (実験的。ビルドで無効なら nativeNtfsMountRw が -EROFS(30) で失敗する)
    @JvmStatic external fun nativeNtfsMountRw(fd: Int): Long
    @JvmStatic external fun nativeNtfsWritable(handle: Long): Boolean
    @JvmStatic external fun nativeNtfsWrite(handle: Long, path: ByteArray, offset: Long, buf: ByteArray, len: Int): Long
    @JvmStatic external fun nativeNtfsCreate(handle: Long, parent: ByteArray, name: ByteArray, isDir: Boolean): Long
    @JvmStatic external fun nativeNtfsRemove(handle: Long, path: ByteArray): Long
    @JvmStatic external fun nativeNtfsRename(handle: Long, path: ByteArray, newName: ByteArray): Long
    @JvmStatic external fun nativeNtfsTruncate(handle: Long, path: ByteArray, size: Long): Long
    @JvmStatic external fun nativeNtfsSync(handle: Long): Long
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

/** NTFS をマウントしたボリューム (既定は読み取り専用)。fd の所有権を引き受ける。[writable] は実際に書き込み可能でマウントされたときだけ true。 */
class NtfsVolume private constructor(private var handle: Long, val writable: Boolean = false) : AutoCloseable {
    private fun bytes(p: String) = p.toByteArray(Charsets.UTF_8)
    private fun checkWritable() { check(handle > 0) { "closed" }; if (!writable) throw NativeException(30, "read-only volume") }
    private fun ok(op: String, r: Long): Long { if (r < 0) throw NativeException((-r).toInt(), "$op failed: errno ${-r}"); return r }

    /** path のファイルへ書く (範囲が EOF を超えれば拡張)。書けたバイト数。 */
    @Synchronized
    fun write(path: String, offset: Long, buf: ByteArray, len: Int = buf.size): Int {
        checkWritable(); return ok("write '$path'", NativeBridge.nativeNtfsWrite(handle, bytes(path), offset, buf, len)).toInt()
    }
    /** parent の下に新規ファイル/ディレクトリを作り、その inode 番号を返す。 */
    @Synchronized
    fun create(parent: String, name: String, isDir: Boolean): Long {
        checkWritable(); return ok("create '$name'", NativeBridge.nativeNtfsCreate(handle, bytes(parent), bytes(name), isDir)).also { sync() }
    }
    @Synchronized
    fun remove(path: String) { checkWritable(); ok("remove '$path'", NativeBridge.nativeNtfsRemove(handle, bytes(path))); sync() }
    @Synchronized
    fun rename(path: String, newName: String) { checkWritable(); ok("rename '$path'", NativeBridge.nativeNtfsRename(handle, bytes(path), bytes(newName))); sync() }
    @Synchronized
    fun truncate(path: String, size: Long) { checkWritable(); ok("truncate '$path'", NativeBridge.nativeNtfsTruncate(handle, bytes(path), size)) }
    /** 変更をメディアへ確定する (書き込み + デバイス flush)。 */
    @Synchronized
    fun sync() { checkWritable(); ok("sync", NativeBridge.nativeNtfsSync(handle)) }

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

    @Synchronized
    fun stat(path: String): NtfsEntry {
        check(handle > 0) { "closed" }
        val err = IntArray(1)
        val b = NativeBridge.nativeNtfsStat(handle, path.toByteArray(Charsets.UTF_8), err)
            ?: throw NativeException(err[0], "stat '$path' failed: errno ${err[0]}")
        val o = org.json.JSONObject(String(b, Charsets.UTF_8))
        return NtfsEntry(path.substringAfterLast('/'), o.getString("ino").toLong(), o.getBoolean("dir"), o.getString("size").toLong(), o.getLong("mtime"))
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
        /** 開いているデバイス(USB 等)の一区間を NTFS として読み取り専用マウント。デバイスの close より先にこの volume を close すること。 */
        fun mountDeviceRange(deviceHandle: Long, offset: Long, length: Long): NtfsVolume {
            val h = NativeBridge.nativeNtfsMountDevice(deviceHandle, offset, length)
            if (h <= 0) throw NativeException((-h).toInt(), "ntfs mount failed: errno ${-h}")
            return NtfsVolume(h)
        }

        /** 書き込み可能で試みる (実験的)。ビルドで無効、またはボリュームがダーティ等で上流が読み取り専用に落とした場合は [writable] = false で返す。 */
        fun mountReadWrite(detachedFd: Int): NtfsVolume {
            val h = NativeBridge.nativeNtfsMountRw(detachedFd)
            if (h <= 0) throw NativeException((-h).toInt(), "ntfs rw mount failed: errno ${-h}")
            return NtfsVolume(h, NativeBridge.nativeNtfsWritable(h))
        }

        fun mountReadOnly(detachedFd: Int): NtfsVolume {
            val h = NativeBridge.nativeNtfsMount(detachedFd)
            if (h <= 0) throw NativeException((-h).toInt(), "ntfs mount failed: errno ${-h}")
            return NtfsVolume(h)
        }
    }
}
