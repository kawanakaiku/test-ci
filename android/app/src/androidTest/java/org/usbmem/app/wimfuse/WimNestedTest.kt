package org.usbmem.app.wimfuse

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.DocumentsContract
import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.usbmem.app.NativeBridge
import org.usbmem.app.VolumeRegistry
import java.io.File
import java.io.FileInputStream

/**
 * 段階 B: ボリューム内の WIM を、ネスト (ループバック) の経路で読み取り専用に公開する (VolumeRegistry.inspectWim / mountWim と SAF プロバイダ)。
 * ホストが tools/fuseshim/gen_wim_fixture.py の parent.img (ext2。/wims/multi.wim = 2 イメージ、/wims/lzx.wim = 1 イメージ、/wims/plain.txt) を /data/local/tmp に置く。
 */
class WimNestedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = VolumeRegistry.OWN_AUTHORITY
    private val entries = ArrayList<String>()
    private val files = ArrayList<File>()

    @After fun cleanup() {
        for (id in entries.reversed()) try { VolumeRegistry.remove(ctx, id) } catch (_: Exception) {}
        for (f in files) f.delete()
    }

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun parent(): VolumeRegistry.Entry {
        assumeTrue("個人版の FUSE 層が入っていない", WimFuse.available(ctx))
        val img = File(ctx.cacheDir, "wimparent.img").also { files += it }
        assumeTrue("フィクスチャ無し", shellCopy("/data/local/tmp/wimfuse_parent.img", img))
        val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
        return VolumeRegistry.addFd("WimParent", "wim-parent", pfd.detachFd(), writable = false, createOnly = false).also { entries += it.id }
    }

    private fun childCount(): Int = try {
        val p = ProcessBuilder("ps", "-A").redirectErrorStream(true).start()
        val n = p.inputStream.bufferedReader().readLines().count { it.contains("libwimfuse") }
        p.waitFor(); n
    } catch (e: Exception) { -1 }

    private fun waitChildCount(n: Int, ms: Long = 8000): Int {
        val end = SystemClock.elapsedRealtime() + ms
        var c = childCount()
        while (c != n && c >= 0 && SystemClock.elapsedRealtime() < end) { Thread.sleep(100); c = childCount() }
        return c
    }

    /** 一時コピーが作られていないことの確認用: アプリが書ける場所のファイル一覧 (パスとサイズ)。 */
    private fun snapshot(): Set<String> {
        val roots = listOfNotNull(ctx.cacheDir, ctx.filesDir, ctx.codeCacheDir, ctx.externalCacheDir, File(System.getProperty("java.io.tmpdir") ?: "/data/local/tmp/none"))
        val out = HashSet<String>()
        for (r in roots) r.walkTopDown().filter { it.isFile }.forEach { out += "${it.path}:${it.length()}" }
        return out
    }

    @Test fun inspect_wimAndNonWim() {
        val p = parent()
        val info = VolumeRegistry.inspectWim(ctx, p.id, "/wims/multi.wim")
        assertNotNull(info)
        assertEquals(2, info!!.imageCount)
        assertEquals(listOf("first", "second image"), info.images.map { it.name })
        assertEquals(1, VolumeRegistry.inspectWim(ctx, p.id, "/wims/lzx.wim")!!.imageCount)
        assertNull("WIM でないファイルは null (パーティション表の経路へ)", VolumeRegistry.inspectWim(ctx, p.id, "/wims/plain.txt"))
        assertEquals("一覧用の子は終わっている", 0, waitChildCount(0))
    }

    @Test fun mountAllImages_asFolders_readOnly_noStaging_closesInOrder() {
        val p = parent()
        val before = snapshot()
        val e = VolumeRegistry.mountWim(ctx, p.id, "/wims/multi.wim", null, "multi.wim").also { entries += it.id }
        assertEquals(p.id, e.parentId)
        assertTrue(!e.volume.writable)
        assertEquals(setOf("1 - first", "2 - second image"), e.volume.list("/").map { it.name }.toSet())
        assertEquals("フォルダを開く前は子 (イメージごとのプロセス) が無い", 0, waitChildCount(0))
        val b = ByteArray(64)
        val n = e.volume.read("/2 - second image/same.txt", 0, b)
        assertEquals("same name, different image 2\n", String(b, 0, n))
        assertEquals(1, waitChildCount(1))
        // 書き込みは拒否される (読み取り専用)
        try { e.volume.create("/1 - first", "x.txt", false); fail("read-only") } catch (_: Exception) {}
        // SAF のプロバイダ経由 (ファイルアプリの経路): 子のドキュメントを開いて読む
        val uri = DocumentsContract.buildDocumentUri(auth, "${e.id}:/1 - first/one.txt")
        val got = ctx.contentResolver.openInputStream(uri)!!.use { String(it.readBytes()) }
        assertEquals("image one\n", got)
        assertEquals(2, waitChildCount(2))
        // 親の WIM は、子が開いている間は書き換え・削除できない (ピン)。ここは読み取り専用の親なので、ピンの確認は同じ mountWim の再呼び出しが同じエントリを返すことで代える
        assertEquals(e.id, VolumeRegistry.mountWim(ctx, p.id, "/wims/multi.wim", null, "multi.wim").id)
        assertEquals("読み取り専用の公開は一時コピーを作らない", before, snapshot())
        // 親ごと閉じれば、子 (WimVolume とイメージごとのプロセス) が先に閉じる
        VolumeRegistry.remove(ctx, p.id)
        assertEquals("親を閉じたら子のプロセスが全部終わる", 0, waitChildCount(0))
        assertNull(VolumeRegistry.get(ctx, e.id))
    }

    @Test fun mountSingleImage_rootIsTheImage() {
        val p = parent()
        val e = VolumeRegistry.mountWim(ctx, p.id, "/wims/multi.wim", 2, "multi.wim #2").also { entries += it.id }
        val names = e.volume.list("/").map { it.name }.toSet()
        assertTrue(names.contains("two.txt") && names.contains("same.txt") && names.contains("sub"))
        val b = ByteArray(64)
        val n = e.volume.read("/two.txt", 0, b)
        assertEquals("image two\n", String(b, 0, n))
        VolumeRegistry.remove(ctx, e.id)
        assertEquals(0, waitChildCount(0))
        // 閉じたあと、同じ場所をもう一度公開できる
        val e2 = VolumeRegistry.mountWim(ctx, p.id, "/wims/multi.wim", 1, "multi.wim #1").also { entries += it.id }
        assertTrue(e2.volume.list("/").any { it.name == "one.txt" })
        Log.i("WIMFUSE_METRIC", "nested_mount_ok=1")
    }

    @Test fun badImageNumber_isRefused_andLeavesNothing() {
        val p = parent()
        try { VolumeRegistry.mountWim(ctx, p.id, "/wims/multi.wim", 9, "bad"); fail("image 9") } catch (e: WimFuseException) { assertTrue(e.errno > 0) }
        assertTrue(VolumeRegistry.childrenOf(p.id).isEmpty())
        assertEquals(0, waitChildCount(0))
    }
}
