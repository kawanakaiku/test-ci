package org.usbmem.app

import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream

/**
 * SAF の ContentResolver 経路で NTFS へ書く (実験的な書き込み)。ビルドで USBMEM_NTFS_WRITE が無効なら assume で skip。
 * 結果のイメージは cacheDir/rw_result.img に残し、ホスト側で ntfsfix / ntfs-3g により独立に検証する (tools/run_android_write_test.sh)。
 */
class DocumentsWriteTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = "org.usbmem.app.documents"
    private val cr get() = ctx.contentResolver

    private fun copy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close(); return dst.length() > 0
    }

    private fun pattern(n: Int, seed: Int) = ByteArray(n) { (((it * 7) + seed) and 255).toByte() }
    private fun uri(id: String) = DocumentsContract.buildDocumentUri(auth, id)

    private fun names(parentId: String): Set<String> {
        val out = HashSet<String>()
        cr.query(DocumentsContract.buildChildDocumentsUri(auth, parentId), arrayOf(Document.COLUMN_DISPLAY_NAME), null, null, null)!!.use { while (it.moveToNext()) out += it.getString(0) }
        return out
    }

    /** 実機の RAID に使う「作成のみ」モード: 新規作成と、作ったファイルへの書き込みだけ。既存ファイルの変更・削除・名前変更・切り詰めは拒否され、メディアに触れない。 */
    @Test
    fun createOnlyModeProtectsExistingData() {
        val img = File(ctx.cacheDir, "rw_createonly.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = try { VolumeRegistry.addFd("CoNTFS", "co-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), writable = true, createOnly = true) }
            catch (ex: NativeException) { assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30); throw ex }
        assumeTrue("volume mounted read-only", e.volume.writable)
        val vid = e.id; val root = "$vid:/"
        try {
            // 既存のファイルを 1 つ探す (内容を後で比較する)
            val existing = e.volume.list("/").first { !it.isDir && it.size > 0 }
            val exId = "$vid:/${existing.name}"
            val before = ByteArray(existing.size.toInt().coerceAtMost(4096)).also { e.volume.read("/${existing.name}", 0, it) }
            // 既存ファイル: 書き込みフラグなし、削除・名前変更フラグなし
            cr.query(uri(exId), arrayOf(Document.COLUMN_FLAGS), null, null, null)!!.use { c -> c.moveToFirst(); val f = c.getInt(0)
                assertEquals(0, f and Document.FLAG_SUPPORTS_WRITE); assertEquals(0, f and Document.FLAG_SUPPORTS_DELETE); assertEquals(0, f and Document.FLAG_SUPPORTS_RENAME) }
            // 拒否される操作
            for (m in listOf("w", "wt", "rw")) try { cr.openFileDescriptor(uri(exId), m)?.close(); fail("open '$m' of existing file must be refused") } catch (_: SecurityException) { } catch (_: java.io.FileNotFoundException) { }
            try { DocumentsContract.deleteDocument(cr, uri(exId)); fail("delete must be refused") } catch (_: Exception) { }
            try { DocumentsContract.renameDocument(cr, uri(exId), "renamed.bin"); fail("rename must be refused") } catch (_: Exception) { }
            for (op in listOf<() -> Unit>({ e.volume.remove("/${existing.name}") }, { e.volume.rename("/${existing.name}", "x") }, { e.volume.truncate("/${existing.name}", 0) }, { e.volume.write("/${existing.name}", 0, ByteArray(8), 8) })) {
                try { op(); fail("must be refused") } catch (ex: NativeException) { assertEquals(1, ex.code) }
            }
            assertTrue(e.volume.stat("/${existing.name}").size == existing.size)
            // 許可される操作: 新規フォルダ・新規ファイルの作成と、そのファイルへの書き込み
            val dirUri = DocumentsContract.createDocument(cr, uri(root), Document.MIME_TYPE_DIR, "new_dir")!!
            val fileUri = DocumentsContract.createDocument(cr, dirUri, "application/octet-stream", "new.bin")!!
            cr.query(fileUri, arrayOf(Document.COLUMN_FLAGS), null, null, null)!!.use { c -> c.moveToFirst(); val f = c.getInt(0)
                assertTrue(f and Document.FLAG_SUPPORTS_WRITE != 0); assertEquals(0, f and Document.FLAG_SUPPORTS_DELETE) }
            val data = pattern(100_000, 3)
            cr.openFileDescriptor(fileUri, "wt")!!.use { p -> java.io.FileOutputStream(p.fileDescriptor).use { it.write(data); it.fd.sync() } }
            assertArrayEquals(data, cr.openInputStream(fileUri)!!.use { it.readBytes() })
            // 既存ファイルは変わっていない
            val after = ByteArray(before.size).also { e.volume.read("/${existing.name}", 0, it) }
            assertArrayEquals(before, after)
            assertTrue("new_dir" in names(root))
        } finally { VolumeRegistry.remove(ctx, vid) }
    }

    /** 一覧を見ているクライアント (ファイルアプリ) へ変更が通知されること。通知が無いと、手動で更新するまで一覧が変わらない。 */
    @Test
    fun changesNotifyChildListObservers() {
        val img = File(ctx.cacheDir, "rw_notify.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = try { VolumeRegistry.addFd("NfNTFS", "nf-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), writable = true) }
            catch (ex: NativeException) { assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30); throw ex }
        assumeTrue("volume mounted read-only", e.volume.writable)
        val root = "${e.id}:/"
        val hits = java.util.concurrent.atomic.AtomicInteger()
        val thread = android.os.HandlerThread("obs").also { it.start() }
        val obs = object : android.database.ContentObserver(android.os.Handler(thread.looper)) { override fun onChange(selfChange: Boolean, uri: android.net.Uri?) { hits.incrementAndGet() } }
        // 操作の「前」に件数を取る (通知の配信が操作の戻りより先に終わることがあるため、後で数えると取りこぼす)
        fun <T> notified(what: String, action: () -> T): T {
            val before = hits.get()
            val r = action()
            val deadline = System.currentTimeMillis() + 3000
            while (hits.get() == before && System.currentTimeMillis() < deadline) Thread.sleep(20)
            assertTrue("no change notification after $what", hits.get() > before)
            return r
        }
        try {
            // 子一覧を一度クエリして、カーソルの通知 URI を登録させる (クライアントが一覧を開いた状態)
            val childrenUri = DocumentsContract.buildChildDocumentsUri(auth, root)
            val c = cr.query(childrenUri, arrayOf(Document.COLUMN_DISPLAY_NAME), null, null, null)!!
            cr.registerContentObserver(childrenUri, false, obs)
            c.close()
            val f = notified("create") { DocumentsContract.createDocument(cr, uri(root), "application/octet-stream", "notify.bin")!! }
            notified("write+close") { cr.openFileDescriptor(f, "wt")!!.use { p -> java.io.FileOutputStream(p.fileDescriptor).use { it.write(ByteArray(1000) { 1 }); it.fd.sync() } } }
            val renamed = notified("rename") { DocumentsContract.renameDocument(cr, f, "notify2.bin") }
            assertNotNull("rename returned null (failed)", renamed)
            notified("delete") { DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUri(auth, "${root.trimEnd('/')}/notify2.bin")) }
        } finally { cr.unregisterContentObserver(obs); thread.quitSafely(); VolumeRegistry.remove(ctx, e.id) }
    }

    @Test
    fun createWriteRenameDelete() {
        val img = File(ctx.cacheDir, "rw_result.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = try { VolumeRegistry.addFd("RwNTFS", "rw-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), writable = true) }
            catch (ex: NativeException) { assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30); throw ex }
        assumeTrue("volume mounted read-only", e.volume.writable)
        val vid = e.id
        try {
            val root = "$vid:/"
            // ルートの権限: 作成可
            cr.query(uri(root), arrayOf(Document.COLUMN_FLAGS), null, null, null)!!.use { c -> c.moveToFirst(); assertTrue(c.getInt(0) and Document.FLAG_DIR_SUPPORTS_CREATE != 0) }

            // mkdir + create + 書き込み (EOF 拡張)
            val dirUri = DocumentsContract.createDocument(cr, uri(root), Document.MIME_TYPE_DIR, "saf_dir")
            assertNotNull(dirUri)
            val fileUri = DocumentsContract.createDocument(cr, dirUri!!, "application/octet-stream", "日本語 file.bin")!!
            val data = pattern(300_000, 5)
            cr.openOutputStream(fileUri, "wt")!!.use { it.write(data) }
            cr.openInputStream(fileUri)!!.use { assertArrayEquals(data, it.readBytes()) }

            // 重複名は " (1)" になる
            val dup = DocumentsContract.createDocument(cr, dirUri, "application/octet-stream", "日本語 file.bin")!!
            assertTrue(names("$vid:/saf_dir").contains("日本語 file (1).bin"))

            // 上書き ("wt" = 切り詰め。Android 10 以降 "w" は切り詰めない) は短くなる
            cr.openOutputStream(fileUri, "wt")!!.use { it.write(pattern(1000, 9)) }
            cr.openInputStream(fileUri)!!.use { assertArrayEquals(pattern(1000, 9), it.readBytes()) }

            // "rw" (切り詰めなし) の途中書き込み: 先頭 10 バイトだけ置換
            cr.openFileDescriptor(fileUri, "rw")!!.use { p ->
                java.io.FileOutputStream(p.fileDescriptor).use { it.write(ByteArray(10) { 0x55 }) }
            }
            cr.openInputStream(fileUri)!!.use { val b = it.readBytes(); assertEquals(1000, b.size); assertTrue(b.take(10).all { x -> x == 0x55.toByte() }); assertEquals(pattern(1000, 9)[10], b[10]) }

            // 既存ファイルの書き換え (元の a.txt)
            cr.openOutputStream(uri("$vid:/a.txt"), "wt")!!.use { it.write("replaced by SAF\n".toByteArray()) }

            // rename (同じディレクトリ内)
            val renamed = DocumentsContract.renameDocument(cr, fileUri, "renamed.bin")
            assertNotNull(renamed)
            assertTrue(names("$vid:/saf_dir").containsAll(setOf("renamed.bin", "日本語 file (1).bin")))
            assertFalse(names("$vid:/saf_dir").contains("日本語 file.bin"))

            // delete: ファイルとディレクトリ (空でないディレクトリの削除は失敗する)
            try { DocumentsContract.deleteDocument(cr, dirUri); fail("non-empty dir must not be deleted") } catch (_: Exception) { }
            assertTrue(DocumentsContract.deleteDocument(cr, renamed!!))
            assertTrue(DocumentsContract.deleteDocument(cr, dup))
            assertTrue(DocumentsContract.deleteDocument(cr, dirUri))
            assertFalse(names(root).contains("saf_dir"))
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/dir1/b.txt")))

            // 不正な名前は拒否
            try { DocumentsContract.createDocument(cr, uri(root), "text/plain", "a/b"); fail("slash in name") } catch (_: Exception) { }
            try { DocumentsContract.createDocument(cr, uri(root), "text/plain", ".."); fail("dotdot") } catch (_: Exception) { }
        } finally {
            VolumeRegistry.remove(ctx, vid)   // アンマウント = 最終 sync + ダーティ解除
        }
        // 結果のイメージ (cacheDir/rw_result.img) は消さない: ホスト側が `adb exec-out run-as` で取り出して ntfsfix / ntfs-3g で独立に検証する
    }

    @Test
    fun readOnlyMountStaysReadOnly() {
        val img = File(ctx.cacheDir, "ro_check.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = VolumeRegistry.addFd("RoNTFS", "ro-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())
        try {
            assertFalse(e.volume.writable)
            val root = uri("${e.id}:/")
            try { DocumentsContract.createDocument(cr, root, "text/plain", "x.txt"); fail("create on read-only volume") } catch (_: Exception) { }
            try { DocumentsContract.deleteDocument(cr, uri("${e.id}:/a.txt")); fail("delete on read-only volume") } catch (_: Exception) { }
        } finally { VolumeRegistry.remove(ctx, e.id); img.delete() }
    }
}
