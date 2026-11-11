package org.usbmem.app

import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/** SAF の ContentResolver 経路 (他アプリと同じバインダ経由) で NTFS を読む。 */
class DocumentsProviderTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = "org.usbmem.app.documents"

    private fun copy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close(); return dst.length() > 0
    }

    private fun withVolume(block: (String) -> Unit) {
        val img = File(ctx.cacheDir, "doc_ntfs.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        try {
            val e = VolumeRegistry.addFd("TestNTFS", "test-key", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())
            try { block(e.id) } finally { VolumeRegistry.remove(ctx, e.id) }
        } finally { img.delete() }
    }

    private fun children(parentId: String): Map<String, String> {
        val uri = DocumentsContract.buildChildDocumentsUri(auth, parentId)
        val out = LinkedHashMap<String, String>()
        ctx.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)!!.use {
            while (it.moveToNext()) out[it.getString(1)] = it.getString(0)
        }
        return out
    }

    @Test
    fun rootsChildrenAndContent() = withVolume { vid ->
        ctx.contentResolver.query(DocumentsContract.buildRootsUri(auth), null, null, null, null)!!.use { c ->
            var found = false
            while (c.moveToNext()) if (c.getString(c.getColumnIndexOrThrow("root_id")) == vid) found = true
            assertTrue(found)
        }
        val top = children("$vid:/")
        assertTrue(top.keys.containsAll(setOf("a.txt", "big.bin", "dir1", "日本語.txt")))
        assertEquals(setOf("b.txt", "sub"), children(top["dir1"]!!).keys)

        val md = MessageDigest.getInstance("SHA-256")
        ctx.contentResolver.openInputStream(DocumentsContract.buildDocumentUri(auth, top["big.bin"]!!))!!.use { ins ->
            val b = ByteArray(1 shl 16); var n: Int
            var total = 0L
            while (ins.read(b).also { n = it } > 0) { md.update(b, 0, n); total += n }
            assertEquals(3146505L, total)
        }
        assertEquals("ddbab060252ff918db12bc44d4836ea1670bd958d28f4c4385c3470c8908d15b", md.digest().joinToString("") { "%02x".format(it) })
    }

    @Test
    fun readOnlyAndTraversalRejected() = withVolume { vid ->
        val a = DocumentsContract.buildDocumentUri(auth, "$vid:/a.txt")
        try { ctx.contentResolver.openFileDescriptor(a, "w")?.close(); fail("write must be rejected") } catch (_: Exception) { }
        try { ctx.contentResolver.openFileDescriptor(a, "rw")?.close(); fail("rw must be rejected") } catch (_: Exception) { }
        val bad = DocumentsContract.buildDocumentUri(auth, "$vid:/dir1/../a.txt")
        val c = try { ctx.contentResolver.query(bad, null, null, null, null) } catch (_: Exception) { null }
        assertFalse(c != null && c.count > 0)
        c?.close()
        // 子判定
        val p = DocumentsContract.buildDocumentUri(auth, "$vid:/dir1")
        assertTrue(DocumentsContract.isChildDocument(ctx.contentResolver, p, DocumentsContract.buildDocumentUri(auth, "$vid:/dir1/b.txt")))
        assertFalse(DocumentsContract.isChildDocument(ctx.contentResolver, p, DocumentsContract.buildDocumentUri(auth, "$vid:/a.txt")))
    }
}
