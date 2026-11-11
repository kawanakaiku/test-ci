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
 * 他アプリ(ファイルマネージャ等)に NTFS ボリュームを読み取り専用で見せる SAF プロバイダ。
 * documentId = "<volumeId>:<絶対パス>" (ルートは "<volumeId>:/")。書き込み系はすべて未対応。
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
                .add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY or Root.FLAG_SUPPORTS_IS_CHILD)
                .add(Root.COLUMN_TITLE, e.label)
                .add(Root.COLUMN_DOCUMENT_ID, "${e.id}:/")
                .add(Root.COLUMN_SUMMARY, "NTFS (読み取り専用)")
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

    private fun addRow(c: MatrixCursor, id: String, e: NtfsEntry, nameOverride: String? = null) {
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, nameOverride ?: e.name)
            .add(Document.COLUMN_MIME_TYPE, mimeOf(e))
            .add(Document.COLUMN_SIZE, if (e.isDir) null else e.size)
            .add(Document.COLUMN_LAST_MODIFIED, e.mtimeSec * 1000)
            .add(Document.COLUMN_FLAGS, 0)  // 書き込み・削除・名前変更などは一切許可しない
    }

    private fun fail(e: NativeException, id: String): Nothing = throw FileNotFoundException("$id (errno ${e.code})")

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val (vol, path) = parse(documentId)
        val c = MatrixCursor(projection ?: docCols)
        try {
            val e = vol.volume.stat(path)
            addRow(c, documentId, e, if (path == "/") vol.label else null)
        } catch (ex: NativeException) { fail(ex, documentId) }
        return c
    }

    override fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?, sortOrder: String?): Cursor {
        val (vol, path) = parse(parentDocumentId)
        val c = MatrixCursor(projection ?: docCols)
        try {
            for (e in vol.volume.list(path)) addRow(c, childId(parentDocumentId, e.name), e)
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

    override fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("read-only provider")  // 書き込みモードは拒否
        val (vol, path) = parse(documentId)
        val st = try { vol.volume.stat(path) } catch (ex: NativeException) { fail(ex, documentId) }
        if (st.isDir) throw FileNotFoundException("is a directory")
        val sm = ctx.getSystemService(Context.STORAGE_SERVICE) as StorageManager
        val cb = object : ProxyFileDescriptorCallback() {
            override fun onGetSize(): Long = st.size
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
            override fun onRelease() {}
        }
        return sm.openProxyFileDescriptor(ParcelFileDescriptor.MODE_READ_ONLY, cb, ioHandler)
    }

    override fun shutdown() { ioThread.quitSafely() }
}
