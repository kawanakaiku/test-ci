// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.ProxyFileDescriptorCallback
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.system.ErrnoException
import android.system.OsConstants
import android.webkit.MimeTypeMap
import java.io.FileNotFoundException

/**
 * SAF の [android.provider.DocumentsProvider] (NtfsDocumentsProvider) の下に、マウント中の SMB 共有 ([SmbMounts]) を足す部分。USB のボリュームと同じプロバイダ・同じ authority で、
 * ルートの名前は「<接続名> (SMB)」。ドキュメント ID = "smb-<接続の id>:<パス>" (ルートは "…:/")。USB のボリューム ID ("<12 桁の 16 進>") とは先頭の "smb-" で区別する。
 *
 * ネットワークの待ちは Binder のスレッド / 専用の IO スレッド ([ioHandler]) で起き、メインスレッドは止めない。USB の IO スレッドとは別なので、
 * ネットワークが詰まっても USB の読み書きは止まらない。読み書きの失敗は errno (切断 = ENODEV、応答なし = ETIMEDOUT、そのほか EIO など) でファイルアプリへ返す。
 */
class SmbDocuments(private val ctx: Context, private val authority: String) {
    private val ioThread = lazy { HandlerThread("smb-io").apply { start() } }
    private val ioHandler by lazy { Handler(ioThread.value.looper) }

    fun shutdown() { if (ioThread.isInitialized()) ioThread.value.quitSafely() }

    private val rootCols = arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_SUMMARY, Root.COLUMN_ICON)
    private val docCols = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS)

    companion object {
        fun owns(documentId: String): Boolean = documentId.startsWith(SmbMounts.ID_PREFIX)
    }

    // ---- ルート
    fun addRoots(c: MatrixCursor) {
        SmbMounts.reapDead(ctx)
        for (m in SmbMounts.all()) {
            c.newRow().add(Root.COLUMN_ROOT_ID, m.id)
                .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_IS_CHILD or (if (m.writable) Root.FLAG_SUPPORTS_CREATE else 0))   // ネットワークなので FLAG_LOCAL_ONLY は付けない
                .add(Root.COLUMN_TITLE, m.label)
                .add(Root.COLUMN_DOCUMENT_ID, m.id + ":/")
                .add(Root.COLUMN_SUMMARY, summary(m))
                .add(Root.COLUMN_ICON, android.R.drawable.ic_menu_save)
        }
    }

    /** USB のルートと同じ形の説明 ("SMB 3.1.1 (読み取り専用) · host/share") */
    private fun summary(m: SmbMounts.Mount): String {
        val mode = if (m.writable) "読み書き" else "読み取り専用"
        val sec = (if (m.fs.encrypted) " · 暗号化" else "") + (if (m.fs.signed) " · 署名" else "")
        return "${m.fs.dialect} ($mode)$sec · ${m.conn.target()}"
    }

    // ---- ID
    private class Ref(val mount: SmbMounts.Mount, val path: String)

    /** "<id>:<パス>" を分解する。".." "." を含むパス・知らない共有は拒否 (パストラバーサル対策) */
    private fun parse(documentId: String): Ref {
        val i = documentId.indexOf(':')
        if (i <= 0) throw FileNotFoundException(documentId)
        val m = SmbMounts.get(documentId.substring(0, i)) ?: throw FileNotFoundException(documentId)
        val path = documentId.substring(i + 1)
        if (!path.startsWith("/") || path.split('/').any { it == ".." || it == "." }) throw FileNotFoundException(documentId)
        return Ref(m, path)
    }

    private fun idOf(m: SmbMounts.Mount, path: String) = m.id + ":" + path
    private fun parentIdOf(documentId: String): String {
        val i = documentId.indexOf(':'); val path = documentId.substring(i + 1)
        val up = SmbPaths.parent(path)
        return documentId.substring(0, i + 1) + up
    }

    private fun notifyChildren(parentDocumentId: String) {
        try { ctx.contentResolver.notifyChange(DocumentsContract.buildChildDocumentsUri(authority, parentDocumentId), null) } catch (_: Exception) { }
    }

    private fun mimeOf(e: RemoteEntry): String {
        if (e.isDir) return Document.MIME_TYPE_DIR
        val ext = e.name.substringAfterLast('.', "").lowercase()
        return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }

    private fun flagsOf(m: SmbMounts.Mount, e: RemoteEntry, isRoot: Boolean): Int {
        if (!m.writable) return 0
        var f = if (e.isDir) Document.FLAG_DIR_SUPPORTS_CREATE else Document.FLAG_SUPPORTS_WRITE
        if (!isRoot) f = f or Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_RENAME or Document.FLAG_SUPPORTS_MOVE
        return f
    }

    private fun addRow(c: MatrixCursor, id: String, e: RemoteEntry, flags: Int, nameOverride: String? = null) {
        c.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, nameOverride ?: e.name)
            .add(Document.COLUMN_MIME_TYPE, mimeOf(e))
            .add(Document.COLUMN_SIZE, if (e.isDir) null else e.size)
            .add(Document.COLUMN_LAST_MODIFIED, if (e.mtimeMs > 0) e.mtimeMs else null)
            .add(Document.COLUMN_FLAGS, flags)
    }

    /** 失敗を、プロバイダの約束の例外へ。パスやサーバーの文字列は含めない */
    private fun toIo(e: Throwable, id: String): Exception {
        val k = (e as? SmbException)?.kind ?: SmbErrors.map(e).kind
        return when (k) {
            SmbErrorKind.NotFound, SmbErrorKind.ShareNotFound, SmbErrorKind.InvalidName -> FileNotFoundException(id)
            SmbErrorKind.AccessDenied, SmbErrorKind.ReadOnly -> SecurityException("access denied")
            SmbErrorKind.Exists -> IllegalStateException("already exists")
            SmbErrorKind.NotEmpty -> IllegalStateException("directory not empty")
            SmbErrorKind.Cancelled -> IllegalStateException("cancelled")
            SmbErrorKind.Disconnected, SmbErrorKind.Network -> IllegalStateException("disconnected")
            SmbErrorKind.Timeout -> IllegalStateException("timed out")
            else -> IllegalStateException("smb error: " + k.name)
        }
    }

    private fun errnoOf(e: Throwable): Int = when (SmbErrors.map(e).kind) {
        SmbErrorKind.Disconnected, SmbErrorKind.Network -> OsConstants.ENODEV
        SmbErrorKind.Timeout -> OsConstants.ETIMEDOUT
        SmbErrorKind.NoSpace -> OsConstants.ENOSPC
        SmbErrorKind.AccessDenied, SmbErrorKind.ReadOnly -> OsConstants.EACCES
        SmbErrorKind.NotFound -> OsConstants.ENOENT
        SmbErrorKind.Busy -> OsConstants.EBUSY
        SmbErrorKind.Cancelled -> OsConstants.EINTR
        else -> OsConstants.EIO
    }

    private fun requireWritable(m: SmbMounts.Mount) { if (!m.writable) throw UnsupportedOperationException("read-only mount") }

    private fun checkName(name: String) {
        if (!SmbPaths.validName(name)) throw IllegalArgumentException("bad name")
    }

    // ---- 問い合わせ
    fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val (m, path) = parse(documentId).let { it.mount to it.path }
        val c = MatrixCursor(projection ?: docCols)
        try {
            val e = m.fs.stat(path)
            addRow(c, documentId, e, flagsOf(m, e, path == "/"), if (path == "/") m.label else null)
        } catch (ex: Exception) { throw toIo(ex, documentId) }
        return c
    }

    fun queryChildDocuments(parentDocumentId: String, projection: Array<out String>?): Cursor {
        val (m, path) = parse(parentDocumentId).let { it.mount to it.path }
        val c = MatrixCursor(projection ?: docCols)
        c.setNotificationUri(ctx.contentResolver, DocumentsContract.buildChildDocumentsUri(authority, parentDocumentId))
        try {
            for (e in m.fs.list(path)) addRow(c, idOf(m, SmbPaths.child(path, e.name)), e, flagsOf(m, e, false))
        } catch (ex: Exception) { throw toIo(ex, parentDocumentId) }
        return c
    }

    fun isChildDocument(parentDocumentId: String, documentId: String): Boolean {
        val p1 = try { parse(parentDocumentId) } catch (_: FileNotFoundException) { return false }
        val p2 = try { parse(documentId) } catch (_: FileNotFoundException) { return false }
        if (p1.mount.id != p2.mount.id) return false
        val base = if (p1.path.endsWith("/")) p1.path else p1.path + "/"
        return p2.path.startsWith(base)
    }

    // ---- 作成・削除・名前変更・移動
    fun createDocument(parentDocumentId: String, mimeType: String, displayName: String): String {
        val r = parse(parentDocumentId)
        requireWritable(r.mount)
        checkName(displayName)
        val isDir = mimeType == Document.MIME_TYPE_DIR
        try {
            if (!r.mount.fs.stat(r.path).isDir) throw FileNotFoundException("$parentDocumentId is not a directory")
            // 同名があれば " (1)", " (2)" ... を付けて重複を避ける (DocumentsProvider の約束)
            var name = displayName; var n = 0
            while (r.mount.fs.exists(SmbPaths.child(r.path, name))) {
                n++
                val dot = if (isDir) -1 else displayName.lastIndexOf('.').let { if (it <= 0) -1 else it }
                name = if (dot < 0) "$displayName ($n)" else displayName.substring(0, dot) + " ($n)" + displayName.substring(dot)
                if (n > 1000) throw FileNotFoundException("too many duplicates")
            }
            val p = SmbPaths.child(r.path, name)
            if (isDir) r.mount.fs.mkdir(p) else r.mount.fs.createFile(p)
            notifyChildren(parentDocumentId)
            return idOf(r.mount, p)
        } catch (ex: FileNotFoundException) { throw ex } catch (ex: Exception) { throw toIo(ex, parentDocumentId) }
    }

    fun deleteDocument(documentId: String) {
        val r = parse(documentId)
        requireWritable(r.mount)
        if (r.path == "/") throw UnsupportedOperationException("root")
        try { r.mount.fs.delete(r.path) } catch (ex: Exception) { throw toIo(ex, documentId) }
        notifyChildren(parentIdOf(documentId))
    }

    fun renameDocument(documentId: String, displayName: String): String? {
        val r = parse(documentId)
        requireWritable(r.mount)
        if (r.path == "/") throw UnsupportedOperationException("root")
        checkName(displayName)
        val to = SmbPaths.child(SmbPaths.parent(r.path), displayName)
        try { r.mount.fs.rename(r.path, to) } catch (ex: Exception) { throw toIo(ex, documentId) }
        notifyChildren(parentIdOf(documentId))
        return idOf(r.mount, to)
    }

    fun moveDocument(sourceDocumentId: String, sourceParentDocumentId: String, targetParentDocumentId: String): String {
        val s = parse(sourceDocumentId)
        val t = parse(targetParentDocumentId)
        if (s.mount.id != t.mount.id) throw UnsupportedOperationException("cross-share move")
        requireWritable(s.mount)
        if (s.path == "/") throw UnsupportedOperationException("root")
        // 自分自身や自分の下へは動かせない
        if (t.path == s.path || t.path.startsWith(if (s.path.endsWith("/")) s.path else s.path + "/")) throw IllegalArgumentException("cannot move into itself")
        val to = SmbPaths.child(t.path, SmbPaths.name(s.path))
        try {
            if (!s.mount.fs.stat(t.path).isDir) throw FileNotFoundException("$targetParentDocumentId is not a directory")
            s.mount.fs.rename(s.path, to)
        } catch (ex: FileNotFoundException) { throw ex } catch (ex: Exception) { throw toIo(ex, sourceDocumentId) }
        notifyChildren(sourceParentDocumentId); notifyChildren(targetParentDocumentId)
        return idOf(s.mount, to)
    }

    // ---- 読み書き
    fun openDocument(documentId: String, mode: String, signal: CancellationSignal?): ParcelFileDescriptor {
        val r = parse(documentId)
        val fs = r.mount.fs
        val pfdMode = ParcelFileDescriptor.parseMode(mode)   // "r" / "w"(切り詰め) / "wt" / "wa" / "rw" / "rwt"
        val write = (pfdMode and ParcelFileDescriptor.MODE_WRITE_ONLY) != 0 || (pfdMode and ParcelFileDescriptor.MODE_READ_WRITE) == ParcelFileDescriptor.MODE_READ_WRITE
        val canRead = mode.contains('r')
        if (write) requireWritable(r.mount)
        val truncate = write && (pfdMode and ParcelFileDescriptor.MODE_TRUNCATE) != 0
        signal?.throwIfCanceled()
        // 開く (ここで権限・存在・切断が分かる)。ハンドルは fd を閉じるまで持つ
        var reader: RemoteReader? = null
        var writer: RemoteWriter? = null
        try {
            val st = fs.stat(r.path)
            if (st.isDir) throw FileNotFoundException("is a directory")
            if (write) writer = fs.openWrite(r.path, create = false, truncate = truncate, readToo = canRead)
            if (!write || canRead) reader = fs.openRead(r.path, readAhead = !write)   // 書き込みもするハンドルは先読みしない (書いた内容が読み取りに見えるように)
        } catch (ex: Exception) {
            try { reader?.close() } catch (_: Exception) { }
            try { writer?.close() } catch (_: Exception) { }
            if (ex is FileNotFoundException) throw ex
            throw toIo(ex, documentId)
        }
        val rd = reader; val wr = writer
        val sm = ctx.getSystemService(Context.STORAGE_SERVICE) as StorageManager
        val cb = object : ProxyFileDescriptorCallback() {
            override fun onGetSize(): Long {
                if (wr == null && rd != null) return rd.size
                return try { fs.stat(r.path).size } catch (e: Exception) { throw ErrnoException("onGetSize", errnoOf(e)) }
            }
            override fun onRead(offset: Long, size: Int, data: ByteArray): Int {
                val src = rd ?: throw ErrnoException("onRead", OsConstants.EBADF)
                try { return src.read(offset, data, 0, size) } catch (e: Exception) { throw ErrnoException("onRead", errnoOf(e)) }
            }
            override fun onWrite(offset: Long, size: Int, data: ByteArray): Int {
                val dst = wr ?: throw ErrnoException("onWrite", OsConstants.EBADF)
                try { dst.write(offset, data, 0, size); return size } catch (e: Exception) { throw ErrnoException("onWrite", errnoOf(e)) }
            }
            override fun onFsync() { if (wr != null) try { wr.flush() } catch (e: Exception) { throw ErrnoException("onFsync", errnoOf(e)) } }
            override fun onRelease() {
                try { rd?.close() } catch (_: Exception) { }
                if (wr != null) {
                    try { wr.flush() } catch (_: Exception) { /* 閉じる時の失敗は報告先が無い */ }
                    try { wr.close() } catch (_: Exception) { }
                    notifyChildren(parentIdOf(documentId))   // サイズ・更新日時の変化を一覧へ反映
                }
            }
        }
        val fdMode = if (!write) ParcelFileDescriptor.MODE_READ_ONLY else if (canRead) ParcelFileDescriptor.MODE_READ_WRITE else ParcelFileDescriptor.MODE_WRITE_ONLY
        return sm.openProxyFileDescriptor(fdMode, cb, ioHandler)
    }
}
