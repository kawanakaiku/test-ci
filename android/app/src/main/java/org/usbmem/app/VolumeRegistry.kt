package org.usbmem.app

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.security.MessageDigest

/**
 * マウント済みボリュームの登録簿。プロセス内で共有し、SAF の URI は端末内の private な設定に保存して再マウントする。
 *
 * ネスト (ループバック): ボリューム内のイメージをマウントした子は、親の id を [Entry.parentId] に持つ。**閉じる順は 内 → 外**:
 * 親を閉じる (または置き換える) と、その子 (孫も) を先に閉じてから親を閉じる。docs/nested-mount.md
 *
 * 複数の USB: USB 由来のボリュームは [Entry.deviceName] (UsbDevice.deviceName) を持ち、[removeByDevice] でそのデバイスの分 (ネストした子を含む) だけを閉じる。
 * ボリュームを**閉じる処理 (書き込みの確定など。遅いことがある) はロックの外**で行う: 1 本の USB を閉じている間も、他の USB の SAF アクセスは止まらない。docs/multi-usb.md
 */
object VolumeRegistry {
    /** [parentId] / [backingPath] = ネストした子のとき、親ボリュームの id とイメージファイルのパス。[closer] = ボリュームを閉じた後に行う後始末 (イメージのデバイスを閉じる・親のピン解除)。 */
    class Entry(val id: String, val label: String, val volume: NtfsVolume, val parentId: String? = null, val backingPath: String? = null,
                /** USB 由来 (UsbDevice.deviceName)。USB 以外・ネストした子は null (子は親をたどって、親のデバイスと一緒に閉じる) */
                val deviceName: String? = null,
                internal val closer: (() -> Unit)? = null)

    private val volumes = LinkedHashMap<String, Entry>()
    private var restored = false
    private const val PREFS = "volumes"
    /** このアプリ自身の DocumentsProvider の authority (NtfsDocumentsProvider と同じ) */
    const val OWN_AUTHORITY = "org.usbmem.app.documents"

    private fun idOf(key: String): String =
        MessageDigest.getInstance("SHA-256").digest(key.toByteArray()).take(6).joinToString("") { "%02x".format(it) }

    @Synchronized
    fun all(ctx: Context): List<Entry> { restore(ctx); return volumes.values.toList() }

    @Synchronized
    fun byKey(key: String): Entry? = volumes[idOf(key)]

    /** USB のボリュームの登録キー。1 本のデバイスに複数のパーティションを同時に公開できるよう、開始位置を含める。 */
    fun usbKey(deviceName: String, startBytes: Long): String = "usb:$deviceName@$startBytes"

    /** そのデバイスから公開しているボリューム (直接のもの。ネストした子は含まない)。 */
    @Synchronized
    fun byDevice(deviceName: String): List<Entry> = volumes.values.filter { it.deviceName == deviceName }

    @Synchronized
    fun get(ctx: Context, id: String): Entry? { restore(ctx); return volumes[id] }

    /** このボリュームの子 (直接のもの) */
    @Synchronized
    fun childrenOf(id: String): List<Entry> = volumes.values.filter { it.parentId == id }

    /** SAF で選ばれたイメージを読み取り専用でマウントして登録する。 */
    @Synchronized
    fun addUri(ctx: Context, uri: Uri, writable: Boolean = false, allowNoload: Boolean = false): Entry {
        restore(ctx)
        // このアプリ自身のプロバイダが公開しているファイル: SAF の fd を経由すると、プロバイダの IO スレッドと登録簿のロックでデッドロックしうる。
        // ネイティブ層で親ボリュームのファイルを直接読むネストマウントにする (読み取り専用)
        ownDocument(uri)?.let { (parentId, path) ->
            if (writable) throw NativeException(30, "an image inside a mounted volume can only be opened read-only")
            val size = (volumes[parentId]?.volume ?: throw java.io.FileNotFoundException(uri.toString())).stat(path).size
            return mountNested(ctx, parentId, path, 0, size, path.substringAfterLast('/'), allowNoload)
        }
        val e = mountUri(ctx, uri, writable, allowNoload)
        // 再起動後の復元は常に読み取り専用 (書き込みは起動のたびに明示的に許可させる)
        prefs(ctx).edit().putStringSet("uris", (savedUris(ctx) + uri.toString()).toSet()).apply()
        return e
    }

    /** このアプリ自身のプロバイダの URI なら、(ボリューム id, パス)。そうでなければ null。ドキュメント ID は "<id>:<path>"。 */
    @Synchronized
    fun ownDocument(uri: Uri): Pair<String, String>? {
        if (uri.authority != OWN_AUTHORITY) return null
        val docId = try { android.provider.DocumentsContract.getDocumentId(uri) } catch (_: Exception) { return null }
        val i = docId.indexOf(':')
        if (i <= 0) return null
        val id = docId.substring(0, i)
        val path = docId.substring(i + 1)
        if (!volumes.containsKey(id) || !path.startsWith("/") || path.split('/').any { it == ".." || it == "." }) return null
        return id to path
    }

    /** テスト・内部用: 既にある fd(所有権を渡す)を登録する。永続化しない。 */
    @Synchronized
    fun addFd(label: String, key: String, detachedFd: Int, writable: Boolean = false, createOnly: Boolean = false, allowNoload: Boolean = false): Entry {
        val id = idOf(key)
        volumes.remove(id)?.let { closeEntry(it) }
        val e = Entry(id, label, if (writable) NtfsVolume.mountReadWrite(detachedFd, createOnly) else NtfsVolume.mountReadOnly(detachedFd, allowNoload))
        volumes[id] = e
        return e
    }

    /** マウント済みの NTFS ボリュームを登録する (USB など。永続化しない: 再接続時に再度登録される)。所有権は登録簿に移る。 */
    @Synchronized
    fun addVolume(label: String, key: String, volume: NtfsVolume, deviceName: String? = null): Entry {
        val id = idOf(key)
        volumes.remove(id)?.let { closeEntry(it) }
        return Entry(id, label, volume, deviceName = deviceName).also { volumes[id] = it }
    }

    /**
     * ボリューム [parentId] の中のイメージ [path] のパーティション表を読む (開いて、読んで、すぐ閉じる。何も登録しない)。
     * 表が無く全体が 1 つのファイルシステムなら [PartitionTable.wholeDeviceVolume]。
     */
    @Synchronized
    fun inspectNested(ctx: Context, parentId: String, path: String): PartitionTable {
        restore(ctx)
        val parent = volumes[parentId] ?: throw java.io.FileNotFoundException(parentId)
        parent.volume.openImage(path).use { return it.partitionTable() }
    }

    /**
     * ボリューム [parentId] の中のイメージ [path] の [offsetBytes, +sizeBytes) を、読み取り専用でマウントして登録する (全体なら 0 とイメージのサイズ)。
     * 同じ場所を既にマウントしていれば、それを返す。段数の上限を超える・循環すると NativeException(ELOOP=40)。
     * 子が開いている間、親のそのファイルは書き換え・削除・移動できない (EBUSY)。
     */
    @Synchronized
    fun mountNested(ctx: Context, parentId: String, path: String, offsetBytes: Long, sizeBytes: Long, label: String, allowNoload: Boolean = false): Entry {
        restore(ctx)
        val parent = volumes[parentId] ?: throw java.io.FileNotFoundException(parentId)
        val id = idOf("nested:$parentId:$path:$offsetBytes")
        volumes[id]?.let { return it }
        val img = parent.volume.openImage(path)
        val v = try { img.mount(offsetBytes, sizeBytes, allowNoload) } catch (t: Throwable) { img.close(); throw t }
        parent.volume.pin(path)
        val e = Entry(id, label, v, parentId, path) { img.close(); parent.volume.unpin(path) }
        volumes[id] = e
        return e
    }

    /** キーのボリューム (とその子) を閉じる。閉じる処理はロックの外で行う。 */
    fun removeByKey(key: String) = removeById(idOf(key))

    fun remove(ctx: Context, id: String) = removeById(id)

    private fun removeById(id: String) {
        val victims = synchronized(this) { volumes.remove(id)?.let { takeOut(it) } ?: return }
        closeInOrder(victims)
    }

    /**
     * そのデバイスから公開しているボリュームを、ネストした子ごと全部閉じる。他のデバイスのボリュームには触れない。閉じた数 (子を含む) を返す。
     * USB を抜いた/閉じたときの後始末。閉じる処理 (書き込みの確定) はロックの外なので、他のデバイスの SAF アクセスを止めない。
     */
    fun removeByDevice(deviceName: String): Int {
        val victims = synchronized(this) {
            volumes.values.filter { it.deviceName == deviceName }.toList().flatMap { e -> if (volumes.remove(e.id) != null) takeOut(e) else emptyList() }
        }
        closeInOrder(victims)
        return victims.size
    }

    /** ロック内で呼ぶ。[e] の子 (孫を含む) を登録簿から外し、閉じる順 (内 → 外: 子、そのあと e) に並べて返す。e は呼び出し側が外してある。 */
    private fun takeOut(e: Entry): List<Entry> {
        val out = ArrayList<Entry>()
        fun walk(x: Entry) {
            for (c in volumes.values.filter { it.parentId == x.id }.toList()) { volumes.remove(c.id); walk(c) }
            out += x
        }
        walk(e)
        return out
    }

    /** 内 → 外の順に閉じる: 子 (孫を含む) を先に、そのあとこのボリューム、最後にイメージのデバイス (closer)。1 つの失敗で残りを止めない (最初の例外を最後に投げる)。 */
    private fun closeInOrder(list: List<Entry>) {
        var first: Throwable? = null
        for (x in list) {
            try { x.volume.close() } catch (t: Throwable) { if (first == null) first = t } finally {
                try { x.closer?.invoke() } catch (t: Throwable) { if (first == null) first = t }
            }
        }
        first?.let { throw it }
    }

    /** ロック内で呼ぶ (置き換え時)。 */
    private fun closeEntry(e: Entry) = closeInOrder(takeOut(e))

    /** SAF のルート一覧が変わったことをシステムの Documents UI へ知らせる。 */
    fun notifyRoots(ctx: Context) {
        ctx.contentResolver.notifyChange(android.provider.DocumentsContract.buildRootsUri(OWN_AUTHORITY), null)
    }

    private fun mountUri(ctx: Context, uri: Uri, writable: Boolean = false, allowNoload: Boolean = false): Entry {
        val pfd: ParcelFileDescriptor = ctx.contentResolver.openFileDescriptor(uri, if (writable) "rw" else "r")
            ?: throw java.io.FileNotFoundException(uri.toString())
        val label = uri.lastPathSegment?.substringAfterLast('/') ?: "NTFS"
        return addFd(label, uri.toString(), pfd.detachFd(), writable, allowNoload = allowNoload)
    }

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private fun savedUris(ctx: Context): Set<String> = prefs(ctx).getStringSet("uris", emptySet()) ?: emptySet()

    private fun restore(ctx: Context) {
        if (restored) return
        restored = true
        for (u in savedUris(ctx)) {
            if (Uri.parse(u).authority == OWN_AUTHORITY) continue   // 自分のプロバイダ経由は、起動時には存在しない (開くとデッドロックしうる)
            try { mountUri(ctx, Uri.parse(u)) } catch (_: Exception) { /* 権限失効・削除済みは黙って捨てる */ }
        }
    }
}
