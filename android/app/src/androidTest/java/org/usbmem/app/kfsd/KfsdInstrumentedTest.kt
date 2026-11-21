package org.usbmem.app.kfsd

import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.usbmem.app.NativeException
import org.usbmem.app.NtfsVolume
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * kfsd (別プロセスの FS ドライバ。docs/kfsd.md) の計装テスト (CI の `kfsd` グループ)。
 *  - LOCAL: 個人版の libkfsd.so を :kfsd サービスが起こす (-PusbmemKfsd=true のビルド)
 *  - PLUGIN: 別 APK のプラグイン (org.usbmem.kfsd) を bound service で起こし、socketpair を Binder で渡す
 * 画像は ext4 のフィクスチャ (tools/gen_ext4_fixture.py。/data/local/tmp/ext4_e4_def4k.img)。プロセス内のマウントと RPC のマウントで、ツリーと内容が一致すること、
 * kfsd が死んでも本体が落ちないこと、ボリュームを閉じると kfsd が終わること、kfsd が無ければ ENOPKG (65) になることを確かめる。
 */
class KfsdInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val image = "e4_def4k"
    private var img: File? = null

    @Before fun fixture() {
        val f = File(ctx.cacheDir, "kfsd_$image.img")
        val pfd = inst.uiAutomation.executeShellCommand("cat /data/local/tmp/ext4_$image.img")
        FileInputStream(pfd.fileDescriptor).use { ins -> f.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        assumeTrue("fixture not pushed", f.length() > 0)
        img = f
    }

    @After fun restore() {
        try { Kfsd.apply(ctx, false) } catch (_: Throwable) {}
        img?.delete()
    }

    private fun open(): NtfsVolume = NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img!!, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())

    /** ツリー全体を (種別・パス・サイズ・先頭 8 KiB の sha256) の行にする。 */
    private fun dump(v: NtfsVolume, path: String = "/", out: MutableList<String> = ArrayList()): List<String> {
        for (e in v.list(path).sortedBy { it.name }) {
            val p = (if (path == "/") "" else path) + "/" + e.name
            if (e.isDir) { out += "D $p"; dump(v, p, out) }
            else if (e.isLink) out += "L $p " + v.readlink(p)
            else {
                val b = ByteArray(8192)
                val n = v.read(p, 0, b).coerceAtLeast(0)
                out += "F $p ${e.size} " + MessageDigest.getInstance("SHA-256").digest(b.copyOf(n)).joinToString("") { "%02x".format(it) }.substring(0, 16)
            }
        }
        return out
    }

    private fun sameTree(src: Kfsd.Source) {
        Kfsd.apply(ctx, false)
        val t1 = System.nanoTime()
        val base = open().use { assertEquals("ext4", it.fsName); dump(it) }
        Log.i("KFSD_METRIC", "tree_dump_inproc: ${(System.nanoTime() - t1) / 1_000_000} ms, ${base.size} entries")
        assertTrue("tree is empty", base.size > 100)
        Kfsd.apply(ctx, true, src)
        val before = Kfsd.children().size
        val t0 = System.nanoTime()
        val rpc = open().use { v ->
            assertEquals("ext4", v.fsName)
            assertEquals(false, v.writable)
            dump(v)
        }
        Log.i("KFSD_METRIC", "tree_dump_$src: ${(System.nanoTime() - t0) / 1_000_000} ms, ${rpc.size} entries")
        assertEquals(base, rpc)
        assertEquals("kfsd child was started once", before + 1, Kfsd.children().size)
    }

    // ---- LOCAL (個人版の libkfsd.so)

    @Test fun local_execAndVersion() {
        assumeTrue("libkfsd.so が無い (-PusbmemKfsd=true のビルドではない)", Kfsd.localAvailable(ctx))
        Kfsd.apply(ctx, true, Kfsd.Source.LOCAL)
        val c = Kfsd.control(ctx)
        val v = c.version()
        Log.i("KFSD_METRIC", "version: $v")
        assertTrue(v, v.contains("kfsd") && v.contains("exit 0"))
        assertNotEquals("サービスは別プロセス (:kfsd)", android.os.Process.myPid(), c.hostPid())
    }

    @Test fun local_sameTreeAsInProcess() {
        assumeTrue("libkfsd.so が無い", Kfsd.localAvailable(ctx))
        sameTree(Kfsd.Source.LOCAL)
    }

    @Test fun local_closeEndsChild() {
        assumeTrue("libkfsd.so が無い", Kfsd.localAvailable(ctx))
        Kfsd.apply(ctx, true, Kfsd.Source.LOCAL)
        val v = open()
        val ch = Kfsd.children().last()
        assertTrue(ch.control.alive(ch.pid))
        v.list("/")
        v.close()
        waitFor("kfsd が終わる (kClose → 正常終了)") { !ch.control.alive(ch.pid) }
        assertEquals("kClose で正常終了 (終了コード 0)", 0, ch.control.exitValue(ch.pid))
    }

    @Test fun local_killedKfsdDoesNotCrashApp() {
        assumeTrue("libkfsd.so が無い", Kfsd.localAvailable(ctx))
        Kfsd.apply(ctx, true, Kfsd.Source.LOCAL)
        val v = open()
        v.list("/")
        val ch = Kfsd.children().last()
        ch.control.kill(ch.pid)
        waitFor("kfsd が死ぬ") { !ch.control.alive(ch.pid) }
        try { v.list("/"); fail("kfsd が死んだ後の list が成功した") } catch (e: NativeException) { Log.i("KFSD_METRIC", "after kill: errno ${e.code}") }
        try { v.stat("/lost+found") ; fail("2 回目も失敗する") } catch (e: NativeException) { }
        v.close()   // 死んだボリュームの close でも落ちない
        // 新しいマウントはできる (別の kfsd が起きる)
        open().use { assertTrue(it.list("/").isNotEmpty()) }
    }

    @Test fun local_noLeftovers() {
        assumeTrue("libkfsd.so が無い", Kfsd.localAvailable(ctx))
        Kfsd.apply(ctx, true, Kfsd.Source.LOCAL)
        repeat(3) { open().use { v -> v.list("/") } }
        val kids = Kfsd.children().takeLast(3)
        for (k in kids) waitFor("子が残らない") { !k.control.alive(k.pid) }
    }

    // ---- PLUGIN (別 APK)

    @Test fun plugin_sameTreeAsInProcess() {
        assumeTrue("kfsd のプラグインが入っていない (署名違いも含む)", Kfsd.pluginAvailable(ctx))
        sameTree(Kfsd.Source.PLUGIN)
        val uid = ctx.packageManager.getPackageUid(Kfsd.PLUGIN_PACKAGE, 0)
        assertNotEquals("プラグインは別の uid", android.os.Process.myUid(), uid)
        val c = Kfsd.control(ctx)
        Log.i("KFSD_METRIC", "plugin uid=$uid service pid=${c.hostPid()} my pid=${android.os.Process.myPid()}")
        assertNotEquals(android.os.Process.myPid(), c.hostPid())
    }

    @Test fun plugin_killedKfsdDoesNotCrashApp() {
        assumeTrue("kfsd のプラグインが入っていない", Kfsd.pluginAvailable(ctx))
        Kfsd.apply(ctx, true, Kfsd.Source.PLUGIN)
        val v = open()
        v.list("/")
        val ch = Kfsd.children().last()
        ch.control.kill(ch.pid)
        waitFor("kfsd が死ぬ") { !ch.control.alive(ch.pid) }
        try { v.list("/"); fail("kfsd が死んだ後の list が成功した") } catch (e: NativeException) { }
        v.close()
        open().use { assertTrue(it.list("/").isNotEmpty()) }
    }

    // ---- kfsd が無い

    @Test fun missingKfsd_isEnopkg() {
        // 入っていないパッケージ名のプラグインを選ぶ → kfsd が無い。マウントは ENOPKG (65) で失敗し、アプリは落ちない
        Kfsd.pluginPackage = "org.usbmem.kfsd.absent"
        try {
            Kfsd.apply(ctx, true, Kfsd.Source.PLUGIN)
            assertEquals(false, Kfsd.available(ctx))
            try { open().close(); fail("マウントできてしまった") } catch (e: NativeException) { assertEquals("ENOPKG", Kfsd.ENOPKG, e.code) }
        } finally { Kfsd.pluginPackage = Kfsd.PLUGIN_PACKAGE }
    }

    private fun waitFor(what: String, timeoutMs: Long = 10_000, cond: () -> Boolean) {
        val t0 = System.currentTimeMillis()
        while (!cond()) {
            if (System.currentTimeMillis() - t0 > timeoutMs) throw AssertionError("timeout: $what")
            Thread.sleep(50)
        }
    }
}
