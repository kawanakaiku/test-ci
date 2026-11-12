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
    @JvmStatic external fun nativeUsbLastOpenTrace(): String
    @JvmStatic external fun nativeUsbIdleStop(handle: Long, idleMs: Long): Int
    @JvmStatic external fun nativeUsbSpunDown(handle: Long): Boolean
    @JvmStatic external fun nativeUsbStartStop(handle: Long, start: Boolean): Int
    @JvmStatic external fun nativeUsbTune(handle: Long, chunkKiB: Int, maxTransferKiB: Int): Int
    @JvmStatic external fun nativeBenchRead(handle: Long, offset: Long, totalBytes: Long, bufKiB: Int): Long
    @JvmStatic external fun nativeBenchReadHash(handle: Long, offset: Long, totalBytes: Long, bufKiB: Int, outHash: LongArray): Long
    @JvmStatic external fun nativeUsbOpStats(handle: Long): String
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeErrcName(code: Int): String
    @JvmStatic external fun nativeNtfsMount(fd: Int): Long
    @JvmStatic external fun nativeNtfsMountNoload(fd: Int): Long
    @JvmStatic external fun nativeNtfsMountDeviceNoload(devHandle: Long, offset: Long, length: Long): Long
    @JvmStatic external fun nativeNtfsUnmount(handle: Long)
    @JvmStatic external fun nativeNtfsList(handle: Long, path: ByteArray, errorOut: IntArray): ByteArray?
    @JvmStatic external fun nativeNtfsStat(handle: Long, path: ByteArray, errorOut: IntArray): ByteArray?
    @JvmStatic external fun nativeNtfsRead(handle: Long, path: ByteArray, offset: Long, buf: ByteArray): Long
    // --- 書き込み (実験的。ビルドで無効なら nativeNtfsMountRw が -EROFS(30) で失敗する)
    @JvmStatic external fun nativeNtfsMountRw(fd: Int): Long
    @JvmStatic external fun nativeNtfsMountDeviceRw(devHandle: Long, offset: Long, length: Long): Long
    @JvmStatic external fun nativeFsName(handle: Long): ByteArray
    @JvmStatic external fun nativeNtfsWritable(handle: Long): Boolean
    @JvmStatic external fun nativeNtfsWrite(handle: Long, path: ByteArray, offset: Long, buf: ByteArray, len: Int): Long
    @JvmStatic external fun nativeNtfsCreate(handle: Long, parent: ByteArray, name: ByteArray, isDir: Boolean): Long
    @JvmStatic external fun nativeNtfsRemove(handle: Long, path: ByteArray): Long
    @JvmStatic external fun nativeNtfsRename(handle: Long, path: ByteArray, newName: ByteArray): Long
    @JvmStatic external fun nativeNtfsMove(handle: Long, path: ByteArray, newParent: ByteArray, newName: ByteArray): Long
    @JvmStatic external fun nativeNtfsTruncate(handle: Long, path: ByteArray, size: Long): Long
    @JvmStatic external fun nativeNtfsSync(handle: Long): Long
    // リンク (ext2 のみ。UI・プロバイダからはまだ使わない)
    @JvmStatic external fun nativeFsReadlink(handle: Long, path: ByteArray, err: IntArray): ByteArray?
    @JvmStatic external fun nativeFsSymlink(handle: Long, parent: ByteArray, name: ByteArray, target: ByteArray): Long
    @JvmStatic external fun nativeFsLink(handle: Long, parent: ByteArray, name: ByteArray, existing: ByteArray): Long
}

/**
 * ネイティブ側のハンドルはポインタをそのまま jlong にしたもの。Android 11 以降の arm64 ではヒープのポインタの上位バイトに
 * タグ (0xb4…) が付くため、**符号付きでは負の値になる** (実機 F-51A で「開けたのにエラー扱い」になった不具合の原因)。
 * エラーは小さな負の値 (-4095..0 = -errno / -Errc) だけで表す。0 は「無し / 閉じた」。
 */
object Handle {
    fun isError(h: Long): Boolean = h in -4095L..0L
    fun valid(h: Long): Boolean = h != 0L && !isError(h)
}

class NativeException(val code: Int, message: String) : Exception(message)

/** fd の所有権を引き受ける(close でネイティブ側が閉じる)。 */
class RawImage private constructor(private var handle: Long) : AutoCloseable {
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
        /** 常に読み取り専用で開く(Phase 1)。 */
        fun openReadOnly(detachedFd: Int): RawImage {
            val h = NativeBridge.nativeOpenFd(detachedFd, true)
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                throw NativeException(code, "open failed: ${NativeBridge.nativeErrcName(code)}")
            }
            return RawImage(h)
        }
    }
}

/** [isLink] = シンボリックリンクそのもの (stat はリンクを辿らない。isDir / size はリンク自身の値)。 */
data class NtfsEntry(val name: String, val ino: Long, val isDir: Boolean, val size: Long, val mtimeSec: Long, val isLink: Boolean = false)

/**
 * NTFS をマウントしたボリューム (既定は読み取り専用)。fd の所有権を引き受ける。[writable] は実際に書き込み可能でマウントされたときだけ true。
 *
 * [createOnly] = true (実機の既存データを守る書き込みモード): 許可するのは**新規作成だけ**。
 *  - create: 既存のディレクトリの下に新規ファイル/ディレクトリを作る (ディレクトリ索引への追加は避けられない)。
 *  - write: このセッションで自分が create したファイルへの書き込みだけ。既存ファイルの内容は変えない。
 *  - remove / rename / truncate: 常に拒否 (EPERM)。
 * 制限はこのクラスで強制する (呼び出し側の実装ミスで既存データが変わらないよう、プロバイダではなくここ)。
 */
class NtfsVolume private constructor(private var handle: Long, val writable: Boolean = false, val createOnly: Boolean = false) : AutoCloseable {
    private val createdHere = HashSet<String>()
    /** マウントしたファイルシステム ("ntfs" / "exfat" / "ext2" / "ext4" / "fat" / "iso9660" / "udf"。ext3 も ext4 ドライバなので "ext4"。FAT12/16/32 は "fat")。 */
    val fsName: String = String(NativeBridge.nativeFsName(handle), Charsets.UTF_8)
    /** ext3/ext4 のジャーナルが未再生のまま (再生せず) 読み取り専用で開いている。内容が古い/欠けている可能性がある。 */
    @Volatile var journalIgnored: Boolean = false
    private fun bytes(p: String) = p.toByteArray(Charsets.UTF_8)
    private fun norm(path: String) = "/" + path.split('/').filter { it.isNotEmpty() }.joinToString("/")
    /** このセッションで作成したパスか (createOnly の書き込み許可の判定に使う)。 */
    @Synchronized
    fun isCreatedHere(path: String): Boolean = norm(path) in createdHere
    private fun checkWritable() { check(Handle.valid(handle)) { "closed" }; if (!writable) throw NativeException(30, "read-only volume") }
    private fun denyIfCreateOnly(op: String) { if (createOnly) throw NativeException(1, "$op is not allowed in create-only mode") }
    private fun ok(op: String, r: Long): Long { if (r < 0) throw NativeException((-r).toInt(), "$op failed: errno ${-r}"); return r }

    /** path のファイルへ書く (範囲が EOF を超えれば拡張)。書けたバイト数。 */
    @Synchronized
    fun write(path: String, offset: Long, buf: ByteArray, len: Int = buf.size): Int {
        checkWritable()
        if (createOnly && !isCreatedHere(path)) throw NativeException(1, "write to an existing file is not allowed in create-only mode: '$path'")
        return ok("write '$path'", NativeBridge.nativeNtfsWrite(handle, bytes(path), offset, buf, len)).toInt()
    }
    /** parent の下に新規ファイル/ディレクトリを作り、その inode 番号を返す。 */
    @Synchronized
    fun create(parent: String, name: String, isDir: Boolean): Long {
        checkWritable()
        val ino = ok("create '$name'", NativeBridge.nativeNtfsCreate(handle, bytes(parent), bytes(name), isDir))
        createdHere += norm(parent + "/" + name)
        sync()
        return ino
    }
    @Synchronized
    fun remove(path: String) { checkWritable(); denyIfCreateOnly("remove"); ok("remove '$path'", NativeBridge.nativeNtfsRemove(handle, bytes(path))); sync() }
    /** ディレクトリなら中身ごと削除する (SAF の deleteDocument の約束)。子から順に消し、最後に 1 回だけ sync する。途中で失敗しても、そこまでの削除は整合した状態。 */
    @Synchronized
    fun removeTree(path: String) {
        checkWritable(); denyIfCreateOnly("remove")
        fun rec(p: String, depth: Int) {
            if (depth > 64) throw NativeException(40, "directory nesting too deep: '$p'")
            if (stat(p).isDir) for (c in list(p)) rec(norm("$p/${c.name}"), depth + 1)
            ok("remove '$p'", NativeBridge.nativeNtfsRemove(handle, bytes(p)))
        }
        try { rec(norm(path), 0) } finally { runCatching { sync() } }
    }
    /** path を newParent の下へ newName で移動する (別ディレクトリへも可)。 */
    @Synchronized
    fun move(path: String, newParent: String, newName: String) {
        checkWritable(); denyIfCreateOnly("move")
        ok("move '$path'", NativeBridge.nativeNtfsMove(handle, bytes(path), bytes(newParent), bytes(newName))); sync()
    }
    @Synchronized
    fun rename(path: String, newName: String) { checkWritable(); denyIfCreateOnly("rename"); ok("rename '$path'", NativeBridge.nativeNtfsRename(handle, bytes(path), bytes(newName))); sync() }
    @Synchronized
    fun truncate(path: String, size: Long) { checkWritable(); denyIfCreateOnly("truncate"); ok("truncate '$path'", NativeBridge.nativeNtfsTruncate(handle, bytes(path), size)) }
    /** シンボリックリンクのリンク先 (読み取り専用でも可)。ext2 以外は errno 95。 */
    @Synchronized
    fun readlink(path: String): String {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val b = NativeBridge.nativeFsReadlink(handle, bytes(path), err) ?: throw NativeException(err[0], "readlink '$path' failed: errno ${err[0]}")
        return String(b, Charsets.UTF_8)
    }
    /** parent の下に name のシンボリックリンクを作る (新規作成なので createOnly でも可)。inode 番号を返す。ext2 のみ。 */
    @Synchronized
    fun symlink(parent: String, name: String, target: String): Long {
        checkWritable()
        val ino = ok("symlink '$name'", NativeBridge.nativeFsSymlink(handle, bytes(parent), bytes(name), bytes(target)))
        createdHere += norm(parent + "/" + name)
        sync()
        return ino
    }
    /** 既存の existing へのハードリンクを parent の下に name で作る (既存データに別名を付けるので createOnly では拒否)。ext2 のみ。 */
    @Synchronized
    fun link(parent: String, name: String, existing: String) {
        checkWritable(); denyIfCreateOnly("link")
        ok("link '$name'", NativeBridge.nativeFsLink(handle, bytes(parent), bytes(name), bytes(existing))); sync()
    }
    /** 変更をメディアへ確定する (書き込み + デバイス flush)。 */
    @Synchronized
    fun sync() { checkWritable(); ok("sync", NativeBridge.nativeNtfsSync(handle)) }

    @Synchronized
    fun list(path: String): List<NtfsEntry> {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val b = NativeBridge.nativeNtfsList(handle, path.toByteArray(Charsets.UTF_8), err)
            ?: throw NativeException(err[0], "list '$path' failed: errno ${err[0]}")
        val arr = org.json.JSONArray(String(b, Charsets.UTF_8))
        return (0 until arr.length()).map {
            val o = arr.getJSONObject(it)
            NtfsEntry(o.getString("name"), o.getString("ino").toLong(), o.getBoolean("dir"), o.getString("size").toLong(), o.getLong("mtime"), o.optBoolean("link", false))
        }
    }

    @Synchronized
    fun stat(path: String): NtfsEntry {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val b = NativeBridge.nativeNtfsStat(handle, path.toByteArray(Charsets.UTF_8), err)
            ?: throw NativeException(err[0], "stat '$path' failed: errno ${err[0]}")
        val o = org.json.JSONObject(String(b, Charsets.UTF_8))
        return NtfsEntry(path.substringAfterLast('/'), o.getString("ino").toLong(), o.getBoolean("dir"), o.getString("size").toLong(), o.getLong("mtime"), o.optBoolean("link", false))
    }

    /** off から buf.size バイトまで読む。EOF で 0。 */
    @Synchronized
    fun read(path: String, offset: Long, buf: ByteArray): Int {
        check(Handle.valid(handle)) { "closed" }
        val r = NativeBridge.nativeNtfsRead(handle, path.toByteArray(Charsets.UTF_8), offset, buf)
        if (r < 0) throw NativeException((-r).toInt(), "read '$path' failed: errno ${-r}")
        return r.toInt()
    }

    @Synchronized
    override fun close() {
        if (Handle.valid(handle)) NativeBridge.nativeNtfsUnmount(handle)
        handle = 0
    }

    companion object {
        /** 開いているデバイス(USB 等)の一区間を NTFS として読み取り専用マウント。デバイスの close より先にこの volume を close すること。 */
        fun mountDeviceRange(deviceHandle: Long, offset: Long, length: Long, allowNoload: Boolean = false): NtfsVolume {
            val h = if (allowNoload) NativeBridge.nativeNtfsMountDeviceNoload(deviceHandle, offset, length) else NativeBridge.nativeNtfsMountDevice(deviceHandle, offset, length)
            if (Handle.isError(h)) throw NativeException((-h).toInt(), "ntfs mount failed: errno ${-h}")
            return NtfsVolume(h).also { it.journalIgnored = allowNoload && (it.fsName == "ext4" || it.fsName == "btrfs") }
        }

        /** USB 等の開いているデバイスの一区間を書き込み可能で試みる (実験的)。デバイスも書き込み可能で開いていること。[writable] は実際に書き込み可能なときだけ true。 */
        fun mountDeviceRangeReadWrite(deviceHandle: Long, offset: Long, length: Long, createOnly: Boolean = true): NtfsVolume {
            val h = NativeBridge.nativeNtfsMountDeviceRw(deviceHandle, offset, length)
            if (Handle.isError(h)) throw NativeException((-h).toInt(), "ntfs rw mount failed: errno ${-h}")
            return NtfsVolume(h, NativeBridge.nativeNtfsWritable(h), createOnly)
        }

        /** 書き込み可能で試みる (実験的)。ビルドで無効、またはボリュームがダーティ等で上流が読み取り専用に落とした場合は [writable] = false で返す。 */
        fun mountReadWrite(detachedFd: Int, createOnly: Boolean = false): NtfsVolume {
            val h = NativeBridge.nativeNtfsMountRw(detachedFd)
            if (Handle.isError(h)) throw NativeException((-h).toInt(), "ntfs rw mount failed: errno ${-h}")
            return NtfsVolume(h, NativeBridge.nativeNtfsWritable(h), createOnly)
        }

        fun mountReadOnly(detachedFd: Int, allowNoload: Boolean = false): NtfsVolume {
            val h = if (allowNoload) NativeBridge.nativeNtfsMountNoload(detachedFd) else NativeBridge.nativeNtfsMount(detachedFd)
            if (Handle.isError(h)) throw NativeException((-h).toInt(), "ntfs mount failed: errno ${-h}")
            return NtfsVolume(h).also { it.journalIgnored = allowNoload && (it.fsName == "ext4" || it.fsName == "btrfs") }
        }
    }
}
