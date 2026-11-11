package org.usbmem.app

import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.ProxyFileDescriptorCallback
import android.os.storage.StorageManager
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.system.ErrnoException
import android.system.OsConstants
import android.webkit.MimeTypeMap
import java.io.FileNotFoundException

/**
 * 他アプリ(ファイルマネージャ等)に NTFS ボリュームを見せる SAF プロバイダ。既定は読み取り専用。
 * 書き込み (作成・削除・名前変更・書き込み・切り詰め) は、**書き込み可能でマウントされたボリュームにだけ**許可する (実験的。VolumeRegistry の writable)。
 * documentId = "<volumeId>:<絶対パス>" (ルートは "<volumeId>:/")。
 */
class NtfsDocumentsProvider : DocumentsProvider() {
    private val ioThread by lazy { HandlerThread("ntfs-io").apply { start() } }
    private val ioHandler by lazy { Handler(ioThread.looper) }

    override fun onCreate(): Boolean = true

    private val ctx: Context get() = context!!

    private val rootCols = arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_SUMMARY, Root.COLUMN_ICON)
    private val docCols = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS)

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val c = MatrixCursor(projection ?: rootCols)
        for (e in VolumeRegistry.all(ctx)) {
            c.newRow().add(Root.COLUMN_ROOT_ID, e.id)
                .add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_IS_CHILD or (if (e.volume.writable) Root.FLAG_SUPPORTS_CREATE else 0))
                .add(Root.COLUMN_TITLE, e.label)
                .add(Root.COLUMN_DOCUMENT_ID, "${e.id}:/")
                .add(Root.COLUMN_SUMMARY, if (!e.volume.writable) "NTFS (読み取り専用)" else if (e.volume.createOnly) "NTFS (新規作成のみ・実験的)" else "NTFS (読み書き・実験的)")
                .add(Root.COLUMN_ICON, android.R.drawable.ic_menu_save)
        }
        return c
    }

    /** "<id>:<path>" を分解し、ボリュームを返す。".." や "." を含むパスは拒否(パストラバーサル対策)。 */
    private fun parse(documentId: String): Pair<VolumeRegistry.Entry, String> {
        val i = documentId.indexOf(':')
        if (i <= 0) throw FileNotFoundException(documentId)
        val vol = VolumeRegistry.get(ctx, documentId.substring(0, i)) ?: throw FileNotFoundException(documentId)
        val path = documentId.substring(i + 1)
        if (!path.startsWith("/") || path.split('/').any { it == ".." || it == "." }) throw FileNotFoundException(documentId)
        return vol to path
    }

    private fun childId(parentId: String, name: String) = if (parentId.endsWith("/")) parentId + name else "$parentId/$name"

    private fun mimeOf(e: NtfsEntry): String {
        if (e.isDir) return Document.MIME_TYPE_DIR
        val ext = e.name.substringAfterLast('.', "").lowercase()
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }

    private fun flagsOf(vol: VolumeRegistry.Entry, e: NtfsEntry, isRoot: Boolean, path: String): Int {
        if (!vol.volume.writable) return 0
        var f = 0
        if (e.isDir) f = f or Document.FLAG_DIR_SUPPORTS_CREATE
        // 作成のみモード: 書き込みはこのセッションで作ったファイルだけ。削除・名前変更は出さない
        else if (!vol.volume.createOnly || vol.volume.isCreatedHere(path)) f = f or Document.FLAG_SUPPORTS_WRITE
        if (!isRoot && !vol.volume.createOnly) f = f or Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_RENAME
        return f
    }

    private fun addRow(c: MatrixCursor, id: String, e: NtfsEntry, flags: Int, nameOverride: String? = null) {
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, nameOverride ?: e.name)
            .add(Document.COLUMN_MIME_TYPE, mimeOf(e))
            .add(Document.COLUMN_SIZE, if (e.isDir) null else e.size)
            .add(Document.COLUMN_LAST_MODIFIED, e.mtimeSec * 1000)
            .add(Document.COLUMN_FLAGS, flags)   // 読み取り専用ボリュームでは常に 0
    }

    private fun fail(e: NativeException, id: String): Nothing = throw FileNotFoundException("$id (errno ${e.code})")

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val (vol, path) = parse(documentId)
        val c = MatrixCursor(projection ?: docCols)
        try {
            val e = vol.volume.stat(path)
            addRow(c, documentId, e, flagsOf(vol, e, path == "/", path), if (path == "/") vol.label else null)
        } catch (ex: NativeException) { fail(ex, documentId) }
        return c
    }

    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor {
        val (vol, path) = parse(parentDocumentId)
        val c = MatrixCursor(projection ?: docCols)
        try {
            for (e in vol.volume.list(path)) addRow(c, childId(parentDocumentId, e.name), e, flagsOf(vol, e, false, if (path.endsWith("/")) path + e.name else "$path/${e.name}"))
        } catch (ex: NativeException) { fail(ex, parentDocumentId) }
        return c
    }

    override fun isChildDocument(parentDocumentId: String, documentId: String): Boolean {
        val (v1, p1) = try { parse(parentDocumentId) } catch (_: FileNotFoundException) { return false }
        val (v2, p2) = try { parse(documentId) } catch (_: FileNotFoundException) { return false }
        if (v1.id != v2.id) return false
        val base = if (p1.endsWith("/")) p1 else "$p1/"
        return p2.startsWith(base)
    }

    /** 表示名として安全か (区切り・制御文字・"."/".." を含まない)。NTFS が使えない文字は上流が拒否する。 */
    private fun checkName(name: String) {
        if (name.isEmpty() || name == "." || name == ".." || name.any { it == '/' || it == '\\' || it.code < 0x20 }) throw IllegalArgumentException("bad name")
    }
    private fun requireWritable(vol: VolumeRegistry.Entry) { if (!vol.volume.writable) throw UnsupportedOperationException("read-only volume") }
    private fun requireNotCreateOnly(vol: VolumeRegistry.Entry, what: String) { if (vol.volume.createOnly) throw SecurityException("作成のみモード: ${what}はできません") }
    private fun wrap(ex: NativeException, id: String): Nothing = throw FileNotFoundException("$id (errno ${ex.code})")

    override fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        val (vol, parent) = parse(parentDocumentId)
        requireWritable(vol)
        checkName(displayName)
        val isDir = mimeType == Document.MIME_TYPE_DIR
        try {
            if (!vol.volume.stat(parent).isDir) throw FileNotFoundException("$parentDocumentId is not a directory")
            // 同名があれば " (1)", " (2)" ... を付けて重複を避ける (DocumentsProvider の約束)
            var name = displayName; var n = 0
            while (true) {
                val p = if (parent.endsWith("/")) parent + name else "$parent/$name"
                val exists = try { vol.volume.stat(p); true } catch (e: NativeException) { if (e.code == 2) false else throw e }
                if (!exists) break
                n++
                val dot = if (isDir) -1 else displayName.lastIndexOf('.').let { if (it <= 0) -1 else it }
                name = if (dot < 0) "$displayName ($n)" else displayName.substring(0, dot) + " ($n)" + displayName.substring(dot)
                if (n > 1000) throw FileNotFoundException("too many duplicates")
            }
            vol.volume.create(parent, name, isDir)
            return childId(parentDocumentId, name)
        } catch (ex: NativeException) { wrap(ex, parentDocumentId) }
    }

    override fun deleteDocument(documentId: String) {
        val (vol, path) = parse(documentId)
        requireWritable(vol)
        requireNotCreateOnly(vol, "削除")
        if (path == "/") throw UnsupportedOperationException("root")
        try { vol.volume.remove(path) } catch (ex: NativeException) { wrap(ex, documentId) }
    }

    override fun renameDocument(documentId: String, displayName: String): String? {
        val (vol, path) = parse(documentId)
        requireWritable(vol)
        requireNotCreateOnly(vol, "名前変更")
        if (path == "/") throw UnsupportedOperationException("root")
        checkName(displayName)
        try { vol.volume.rename(path, displayName) } catch (ex: NativeException) { wrap(ex, documentId) }
        return documentId.substringBeforeLast('/') + "/" + displayName   // "<vol>:/a/b" -> "<vol>:/a/new", "<vol>:/b" -> "<vol>:/new"
    }

    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor {
        val (vol, path) = parse(documentId)
        val pfdMode = ParcelFileDescriptor.parseMode(mode)   // "r" / "w"(切り詰め) / "wt" / "wa" / "rw" / "rwt"
        val write = (pfdMode and ParcelFileDescriptor.MODE_WRITE_ONLY) != 0 || (pfdMode and ParcelFileDescriptor.MODE_READ_WRITE) == ParcelFileDescriptor.MODE_READ_WRITE
        val canRead = mode.contains('r')
        if (write) requireWritable(vol)
        if (write && vol.volume.createOnly && !vol.volume.isCreatedHere(path)) throw SecurityException("作成のみモード: 既存のファイルは書き換えられません")
        val st = try { vol.volume.stat(path) } catch (ex: NativeException) { fail(ex, documentId) }
        if (st.isDir) throw FileNotFoundException("is a directory")
        val truncate = write && (pfdMode and ParcelFileDescriptor.MODE_TRUNCATE) != 0
        try { if (truncate && st.size != 0L) vol.volume.truncate(path, 0) } catch (ex: NativeException) { wrap(ex, documentId) }
        val sm = ctx.getSystemService(Context.STORAGE_SERVICE) as StorageManager
        val cb = object : ProxyFileDescriptorCallback() {
            override fun onGetSize(): Long = try { vol.volume.stat(path).size } catch (e: NativeException) { throw ErrnoException("onGetSize", OsConstants.EIO) }
            override fun onRead(offset: Long, size: Int, data: ByteArray): Int {
                try {
                    var done = 0
                    while (done < size) {
                        val chunk = ByteArray(size - done)
                        val n = vol.volume.read(path, offset + done, chunk)
                        if (n <= 0) break  // EOF
                        System.arraycopy(chunk, 0, data, done, n)
                        done += n
                    }
                    return done
                } catch (e: NativeException) { throw ErrnoException("onRead", OsConstants.EIO) }
            }
            override fun onWrite(offset: Long, size: Int, data: ByteArray): Int {
                if (!write) throw ErrnoException("onWrite", OsConstants.EBADF)
                try {
                    var done = 0
                    while (done < size) {
                        val chunk = if (done == 0 && size == data.size) data else data.copyOfRange(done, size)
                        val n = vol.volume.write(path, offset + done, chunk, size - done)
                        if (n <= 0) throw ErrnoException("onWrite", OsConstants.EIO)
                        done += n
                    }
                    return done
                } catch (e: NativeException) { throw ErrnoException("onWrite", if (e.code == 28) OsConstants.ENOSPC else OsConstants.EIO) }
            }
            override fun onFsync() { if (write) try { vol.volume.sync() } catch (e: NativeException) { throw ErrnoException("onFsync", OsConstants.EIO) } }
            override fun onRelease() { if (write) try { vol.volume.sync() } catch (_: Exception) { /* 閉じる時の失敗は報告先が無い。次回マウント時にダーティで検出される */ } }
        }
        val fdMode = if (!write) ParcelFileDescriptor.MODE_READ_ONLY else if (canRead) ParcelFileDescriptor.MODE_READ_WRITE else ParcelFileDescriptor.MODE_WRITE_ONLY
        return sm.openProxyFileDescriptor(fdMode, cb, ioHandler)
    }

    override fun shutdown() { ioThread.quitSafely() }
}
