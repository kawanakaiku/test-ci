package org.usbmem.app.wimfuse

import android.content.Context
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.usbmem.app.NativeBridge
import org.usbmem.app.NtfsVolume
import java.io.File
import java.security.MessageDigest

/**
 * 個人版の FUSE 層 (別プロセスの wimlib。docs/fuse-layer.md 10 章) の計装テスト (CI の `wim-fuse` グループ)。
 * フィクスチャ (tools/fuseshim/gen_wim_fixture.py): /data/local/tmp/wimfuse_{lzx,none,lzms}.wim と wimfuse_expected.txt (元のツリーの 種別・サイズ・sha256・パス)。
 * WIM は、アプリのプロセスで開いた BlockDevice (RawImage) として子プロセスに RPC 越しに読ませる (子は WIM ファイルを open しない)。
 */
class WimFuseTest {
    private val ctx: Context = InstrumentationRegistry.getInstrumentation().targetContext
    private val tmp = "/data/local/tmp"
    private val opened = ArrayList<AutoCloseable>()

    @Before fun requireBuild() {
        assumeTrue("wimfuse がビルドに入っていない (-PusbmemWimFuse)", WimFuse.available(ctx))
        for (n in listOf("wimfuse_lzx.wim", "wimfuse_none.wim", "wimfuse_expected.txt")) assumeTrue("フィクスチャ無し: $n", File("$tmp/$n").isFile)
    }

    @After fun cleanup() { opened.reversed().forEach { try { it.close() } catch (_: Exception) {} } }

    private fun metric(key: String, value: String) {
        Log.i("WIMFUSE_METRIC", "$key=$value")
        val b = Bundle().apply { putString("wimfuse_$key", value) }
        InstrumentationRegistry.getInstrumentation().sendStatus(0, b)
    }

    /** WIM ファイルを device ハンドルで開く (アプリのプロセスが読む。子は RPC 越しにこれを読む)。 */
    private fun openDevice(name: String): Long {
        val pfd = ParcelFileDescriptor.open(File("$tmp/$name"), ParcelFileDescriptor.MODE_READ_ONLY)
        val dev = NativeBridge.nativeOpenFd(pfd.detachFd(), true)
        assertTrue("nativeOpenFd=$dev", dev > 0)
        opened += AutoCloseable { NativeBridge.nativeClose(dev) }
        return dev
    }

    private fun expected(): Set<String> = File("$tmp/wimfuse_expected.txt").readLines().filter { it.isNotBlank() }.toSet()

    /** ツリー全体を (種別・サイズ・sha256・パス) の行にする。fsdump / gen_wim_fixture.py の形式。 */
    private fun dump(v: NtfsVolume): Pair<Set<String>, Long> {
        val out = HashSet<String>()
        var bytes = 0L
        val buf = ByteArray(1 shl 20)
        fun walk(dir: String) {
            for (e in v.list(dir)) {
                if (e.name == "." || e.name == "..") continue
                val p = (if (dir == "/") "" else dir) + "/" + e.name
                if (e.isDir) { out += "d - 0 - $p"; walk(p) }
                else {
                    val md = MessageDigest.getInstance("SHA-256")
                    var off = 0L
                    while (true) {
                        val n = v.read(p, off, buf)
                        if (n <= 0) break
                        md.update(buf, 0, n); off += n
                    }
                    assertEquals("size of $p", e.size, off)
                    bytes += off
                    out += "f ${e.size} 0 ${md.digest().joinToString("") { "%02x".format(it) }} $p"
                }
            }
        }
        walk("/")
        return out to bytes
    }

    private fun alive(pid: Int): Boolean = try { Os.kill(pid, 0); true } catch (e: ErrnoException) { e.errno != OsConstants.ESRCH }

    private fun waitGone(pid: Int, ms: Long = 8000): Boolean {
        val end = SystemClock.elapsedRealtime() + ms
        while (SystemClock.elapsedRealtime() < end) { if (!alive(pid)) return true; Thread.sleep(50) }
        return !alive(pid)
    }

    private fun procStatus(pid: Int, key: String): String = try {
        File("/proc/$pid/status").readLines().firstOrNull { it.startsWith("$key:") }?.substringAfter(':')?.trim() ?: "n/a"
    } catch (e: Exception) { "n/a(${e.javaClass.simpleName})" }

    // ---- (a) 子プロセスが起動する ----

    @Test fun execFromNativeLibraryDir_runsInBothModes() {
        val exe = WimFuse.exeFile(ctx)
        metric("native_lib_dir", ctx.applicationInfo.nativeLibraryDir)
        metric("exe_size", exe.length().toString())
        assertTrue("canExecute ${exe.path}", exe.canExecute())
        for (m in WimFuse.Mode.values()) {
            val t0 = SystemClock.elapsedRealtime()
            val v = WimFuse.version(ctx, m)
            metric("version_${m.name.lowercase()}", v + " ${SystemClock.elapsedRealtime() - t0}ms")
            assertTrue(v, v.contains("wimlib 1.14.4") && v.endsWith("(exit 0)"))
        }
    }

    @Test fun serviceRunsInSeparateProcess_childIsItsChild() {
        val dev = openDevice("wimfuse_none.wim")
        WimFuse.open(ctx, dev, 1, WimFuse.Mode.SERVICE).use { img ->
            val host = img.control.hostPid()
            val me = android.os.Process.myPid()
            metric("pids", "app=$me service=$host child=${img.childPid}")
            assertNotEquals("サービスは別プロセス", me, host)
            assertNotEquals(host, img.childPid)
            assertTrue(img.childAlive())
            // 子の親はサービスのプロセス (読めれば)
            val ppid = procStatus(img.childPid, "PPid")
            metric("child_ppid", ppid)
            if (!ppid.startsWith("n/a")) assertEquals(host.toString(), ppid)
            metric("child_vmhwm_mounted", procStatus(img.childPid, "VmHWM"))
            val st = img.volume.stat("/a.txt")
            assertEquals(6L, st.size)
        }
    }

    // ---- (b) イメージ一覧・ツリー・内容が元の WIM のツリーと一致する ----

    @Test fun info_listsImages() {
        for (n in listOf("lzx", "none")) {
            val dev = openDevice("wimfuse_$n.wim")
            val t0 = SystemClock.elapsedRealtime()
            val info = WimFuse.info(ctx, dev)
            metric("info_${n}_ms", (SystemClock.elapsedRealtime() - t0).toString())
            assertEquals(1, info.imageCount)
            assertEquals("img", info.images[0].name)
            assertEquals(1, info.images[0].index)
            assertTrue("files=${info.images[0].files}", info.images[0].files >= 809)
            assertEquals(if (n == "lzx") "LZX" else "None", info.compression)
            assertFalse(info.split)
        }
    }

    private fun treeMatches(name: String, mode: WimFuse.Mode) {
        val dev = openDevice("wimfuse_$name.wim")
        val t0 = SystemClock.elapsedRealtime()
        WimFuse.open(ctx, dev, 1, mode).use { img ->
            metric("open_${name}_${mode.name.lowercase()}_ms", (SystemClock.elapsedRealtime() - t0).toString())
            metric("open_${name}_${mode.name.lowercase()}_timings", img.timings.toString())
            val t1 = SystemClock.elapsedRealtime()
            val (got, bytes) = dump(img.volume)
            val ms = SystemClock.elapsedRealtime() - t1
            val exp = expected()
            val missing = exp - got
            val extra = got - exp
            assertTrue("元のツリーに無い/足りない: missing=${missing.take(5)} extra=${extra.take(5)} (got=${got.size} exp=${exp.size})", missing.isEmpty() && extra.isEmpty())
            metric("tree_${name}_${mode.name.lowercase()}", "${got.size} entries, $bytes bytes, ${ms}ms, ${"%.1f".format(bytes / 1048576.0 / (ms / 1000.0))} MiB/s")
            metric("child_vmhwm_${name}_${mode.name.lowercase()}", procStatus(img.childPid, "VmHWM"))
            // 連続読みの速度 (32 MiB のファイル)
            val buf = ByteArray(1 shl 20)
            val t2 = SystemClock.elapsedRealtime()
            var off = 0L
            while (true) { val n = img.volume.read("/big.bin", off, buf); if (n <= 0) break; off += n }
            val ms2 = SystemClock.elapsedRealtime() - t2
            metric("seqread_${name}_${mode.name.lowercase()}", "$off bytes ${ms2}ms ${"%.1f".format(off / 1048576.0 / (ms2 / 1000.0))} MiB/s")
            assertEquals(32L * 1024 * 1024, off)
        }
    }

    @Test fun treeMatchesOriginal_lzx() = treeMatches("lzx", WimFuse.Mode.SERVICE)
    @Test fun treeMatchesOriginal_none() = treeMatches("none", WimFuse.Mode.SERVICE)
    @Test fun treeMatchesOriginal_lzx_direct() = treeMatches("lzx", WimFuse.Mode.DIRECT)

    @Test fun readOnly_noStagingDirectory() {
        val before = File(tmp).list()?.toSet() ?: emptySet()
        val dev = openDevice("wimfuse_lzx.wim")
        WimFuse.open(ctx, dev, 1).use { img ->
            assertTrue(img.volume.list("/").isNotEmpty())
            val during = (File(tmp).list()?.toSet() ?: emptySet()) - before
            assertTrue("一時ファイル / staging が作られた: $during", during.none { it.contains("wimlib") || it.contains("stage") })
            assertFalse(img.volume.writable)
            // 子の cwd・ファイル記述子に WIM のコピーが無いこと (開いているのは /dev/null と ソケット等)
            val fds = try { File("/proc/${img.childPid}/fd").listFiles()?.map { Os.readlink(it.path) } ?: emptyList() } catch (e: Exception) { emptyList() }
            metric("child_fds", fds.toString())
            assertTrue("WIM / 一時ファイルを実ファイルとして開いている: $fds", fds.none { it.startsWith("/data/") })
        }
    }

    // ---- 子プロセスの後始末 ----

    @Test fun childExitsWhenVolumeClosed() {
        val dev = openDevice("wimfuse_none.wim")
        val img = WimFuse.open(ctx, dev, 1)
        val pid = img.childPid
        assertTrue(alive(pid))
        img.close()
        assertTrue("ボリュームを閉じても子 $pid が残っている", waitGone(pid))
    }

    @Test fun childExitsWhenServiceProcessDies() {
        val dev = openDevice("wimfuse_none.wim")
        val img = WimFuse.open(ctx, dev, 1, WimFuse.Mode.SERVICE, pdeathsig = true)
        val pid = img.childPid
        val host = img.control.hostPid()
        assertTrue(alive(pid))
        android.os.Process.killProcess(host)   // :wimfuse を落とす (本体は生きたまま)
        assertTrue("サービスが死んでも子 $pid が残っている", waitGone(pid))
        // 子が死んだあとの本体側の読みは、フリーズせず EPIPE 系のエラーで返る
        val r = try { img.volume.read("/a.txt", 0, ByteArray(16)); "ok" } catch (e: Exception) { "error: ${e.message}" }
        metric("read_after_child_death", r)
        assertTrue(r, r.startsWith("error"))
    }

    @Test fun childExitsWhenParentSideClosesWithoutQuit() {
        // 本体が QUIT も送らずに接続を失った (本体の異常終了の再現): 子は EOF で終わる
        val dev = openDevice("wimfuse_none.wim")
        val img = WimFuse.open(ctx, dev, 1, WimFuse.Mode.DIRECT, pdeathsig = false)
        val pid = img.childPid
        img.volume.close()   // Vol を放す = ソケットを閉じる
        assertTrue("EOF で子 $pid が終わらない", waitGone(pid))
        img.close()
    }

    @Test fun missingFile_returnsEnoent() {
        val dev = openDevice("wimfuse_none.wim")
        WimFuse.open(ctx, dev, 1).use { img ->
            try { img.volume.stat("/no-such-file"); throw AssertionError("stat が成功した") } catch (e: org.usbmem.app.NativeException) { assertEquals(2, e.code) }
        }
    }

    @Test fun badImageNumber_isRefused() {
        val dev = openDevice("wimfuse_none.wim")
        try { WimFuse.open(ctx, dev, 7).close(); throw AssertionError("存在しないイメージがマウントできた") } catch (e: WimFuseException) { metric("bad_image_error", e.message ?: "") }
    }
}
