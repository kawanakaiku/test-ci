package org.usbmem.app

import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream

/**
 * ext2 上のシンボリックリンク/ハードリンクをファイルアプリ (SAF プロバイダ) からどう見えるか。ホストが /data/local/tmp/ext2_wr.img (b1k) を置く。
 * 相対/絶対/".." で上へ抜ける/ディレクトリへのリンク/壊れたリンク/循環 (a→b→a、自分自身、祖先)、リンクの削除・名前変更・移動がリンク先を壊さないこと、リンク越しの書き込み。
 */
class LinkProviderTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = "org.usbmem.app.documents"
    private val cr get() = ctx.contentResolver

    private fun copy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close(); return dst.length() > 0
    }

    private fun uri(id: String) = DocumentsContract.buildDocumentUri(auth, id)
    private data class Row(val name: String, val mime: String, val size: Long)
    private fun row(id: String): Row = cr.query(uri(id), arrayOf(Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE), null, null, null)!!.use {
        assertTrue("no row for $id", it.moveToFirst()); Row(it.getString(0), it.getString(1), if (it.isNull(2)) -1 else it.getLong(2))
    }
    private fun children(id: String): Map<String, Row> {
        val out = LinkedHashMap<String, Row>()
        cr.query(DocumentsContract.buildChildDocumentsUri(auth, id), arrayOf(Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE), null, null, null)!!.use {
            while (it.moveToNext()) out[it.getString(0)] = Row(it.getString(0), it.getString(1), if (it.isNull(2)) -1 else it.getLong(2))
        }
        return out
    }
    private fun read(id: String): ByteArray = cr.openInputStream(uri(id))!!.use { it.readBytes() }

    @Test
    fun relativePathHelper() {
        assertEquals("../c/d", LinkResolver.relative("/a/b", "/a/c/d"))
        assertEquals("x", LinkResolver.relative("/a", "/a/x"))
        assertEquals(".", LinkResolver.relative("/a/b", "/a/b"))
        assertEquals("../..", LinkResolver.relative("/a/b", "/"))
        assertEquals("a/b", LinkResolver.relative("/", "/a/b"))
    }

    @Test
    fun linksAppearAndBehave() {
        val img = File(ctx.cacheDir, "links.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ext2_wr.img", img))
        val e = try {
            VolumeRegistry.addFd("LinkExt2", "link-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), writable = true, createOnly = false)
        } catch (ex: NativeException) { assumeTrue("write support disabled (errno ${ex.code})", ex.code != 30); throw ex }
        assumeTrue("not ext2", e.volume.fsName == "ext2")
        val v = e.volume; val vid = e.id
        try {
            val real = "hello real file\n".toByteArray()
            v.create("/", "lk", true)
            v.create("/lk", "real.txt", false); v.write("/lk/real.txt", 0, real)
            v.create("/lk", "d", true); v.create("/lk/d", "inner.txt", false); v.write("/lk/d/inner.txt", 0, "inner".toByteArray())
            v.symlink("/lk", "rel", "real.txt")
            v.symlink("/lk", "dl", "d")
            v.symlink("/lk", "abs", "/lk/real.txt")
            v.symlink("/lk", "up", "../lk/real.txt")
            v.symlink("/lk", "esc", "../../../../lk/real.txt")      // ルートより上へは行かない (ルートに丸める)
            v.symlink("/lk", "dangling", "nope")
            v.symlink("/lk", "loop1", "loop2"); v.symlink("/lk", "loop2", "loop1")
            v.symlink("/lk", "self", ".")
            v.symlink("/lk/d", "back", "..")
            v.link("/lk", "hard", "/lk/real.txt")

            val kids = children("$vid:/lk")
            // 実体と同じに見える: ファイルはサイズ・中身が同じ、ディレクトリへのリンクはディレクトリ
            for (n in listOf("rel", "abs", "up", "esc", "hard")) {
                assertEquals("size of $n", real.size.toLong(), kids[n]!!.size)
                assertFalse("mime of $n", kids[n]!!.mime == Document.MIME_TYPE_DIR)
                assertArrayEquals("content of $n", real, read("$vid:/lk/$n"))
            }
            assertEquals(Document.MIME_TYPE_DIR, kids["dl"]!!.mime)
            assertEquals(setOf("inner.txt", "back"), children("$vid:/lk/dl").keys)
            assertArrayEquals("inner".toByteArray(), read("$vid:/lk/dl/inner.txt"))
            // 壊れた/循環するリンクは、リンク先の文字列を中身とする小さなファイル (辿らない)
            for ((n, t) in mapOf("dangling" to "nope", "loop1" to "loop2", "loop2" to "loop1", "self" to ".")) {
                assertFalse("mime of $n", kids[n]!!.mime == Document.MIME_TYPE_DIR)
                assertEquals("size of $n", t.length.toLong(), kids[n]!!.size)
                assertArrayEquals("content of $n", t.toByteArray(), read("$vid:/lk/$n"))
            }
            assertFalse(children("$vid:/lk/d")["back"]!!.mime == Document.MIME_TYPE_DIR)   // 祖先を指すリンクは辿らない
            try { cr.openOutputStream(uri("$vid:/lk/dangling"), "w")!!.use { it.write(1) }; fail("write to broken link must fail") } catch (_: Exception) { }

            // リンク越しの書き込みは実体に届く
            cr.openOutputStream(uri("$vid:/lk/abs"), "wt")!!.use { it.write("changed\n".toByteArray()) }
            assertArrayEquals("changed\n".toByteArray(), read("$vid:/lk/real.txt"))
            assertArrayEquals("changed\n".toByteArray(), read("$vid:/lk/hard"))
            // リンク先のフォルダの中に作る
            val made = DocumentsContract.createDocument(cr, uri("$vid:/lk/dl"), "text/plain", "via_link.txt")!!
            assertTrue(v.stat("/lk/d/via_link.txt").size == 0L)
            assertTrue(DocumentsContract.deleteDocument(cr, made))

            // 削除・名前変更: リンク自身だけ。リンク先は無傷
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/lk/dl")))
            assertTrue("target dir must survive", v.stat("/lk/d").isDir)
            assertEquals(5L, v.stat("/lk/d/inner.txt").size)
            assertFalse(v.list("/lk").any { it.name == "dl" })
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/lk/rel")))
            assertEquals("changed\n".length.toLong(), v.stat("/lk/real.txt").size)
            val renamed = DocumentsContract.renameDocument(cr, uri("$vid:/lk/abs"), "abs2")
            assertTrue(renamed != null)
            assertEquals("/lk/real.txt", v.readlink("/lk/abs2"))   // リンクの中身は変わらない
            // ハードリンク: 片方を消しても中身は残る
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/lk/real.txt")))
            assertArrayEquals("changed\n".toByteArray(), read("$vid:/lk/hard"))
            // 壊れたリンクも消せる
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/lk/dangling")))
            assertFalse(v.list("/lk").any { it.name == "dangling" })
            // 再帰削除は、中のリンクを辿らない
            v.symlink("/lk/d", "toroot", "/")
            assertTrue(DocumentsContract.deleteDocument(cr, uri("$vid:/lk/d")))
            assertTrue("volume root contents must survive", v.list("/").any { it.name == "lk" })
        } finally { VolumeRegistry.remove(ctx, e.id); img.delete() }
    }

    @Test
    fun ntfsBehaviourUnchanged() {
        val img = File(ctx.cacheDir, "links_ntfs.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = VolumeRegistry.addFd("N", "link-ntfs", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd(), writable = false)
        try {
            assertFalse(LinkResolver.supported(e.volume))
            assertEquals("/a/b", LinkResolver.physical(e.volume, "/a/b", true))
        } finally { VolumeRegistry.remove(ctx, e.id); img.delete() }
    }
}
