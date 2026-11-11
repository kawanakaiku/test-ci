package org.usbmem.app

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream

/**
 * 実 ext4 イメージ (tools/gen_ext4_fixture.py) を Android 上で fs/ext4 経由で読む (読み取り専用・ジャーナルは再生しない)。
 * 事前に adb push で /data/local/tmp/ext4_<name>.{img,manifest} を置く (name = e4_def4k / e4_inline1k / e4_gdtcsum4k / e3_4k)。
 * 拒否の確認用に ext4_e4_needs_recovery.img (先頭 4 MiB で足りる)・ext4_rej_encrypt.img / ext4_rej_casefold.img / ext4_rej_verity.img は .img のみ。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target (形式は Ext2InstrumentedTest と同じ)。
 */
class Ext4InstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun want(seed: Int, i: Long): Int = ((i * 31 + seed * 7 + (i shr 8)) and 0xFF).toInt()

    /** [off, off+len) を読み、パターン (seed >= 0) またはゼロ (seed < 0) と一致することを確かめる。 */
    private fun checkRange(vol: NtfsVolume, path: String, off: Long, len: Long, seed: Int) {
        var pos = off
        val end = off + len
        while (pos < end) {
            val buf = ByteArray(minOf(65536L, end - pos).toInt())
            val n = vol.read(path, pos, buf)
            if (n <= 0) throw AssertionError("$path @$pos: short read")
            for (k in 0 until n) cmp(path, pos + k, buf[k].toInt() and 0xFF, seed)
            pos += n
        }
    }
    private fun cmp(path: String, i: Long, got: Int, seed: Int) {
        val w = if (seed >= 0) want(seed, i) else 0
        if (got != w) throw AssertionError("$path @$i: $got != $w")
    }

    private fun verify(name: String) {
        val img = File(ctx.cacheDir, "ext4_$name.img")
        val man = File(ctx.cacheDir, "ext4_$name.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_$name.manifest", man) && shellCopy("/data/local/tmp/ext4_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("ext4", vol.fsName)
                assertFalse(vol.writable)
                var files = 0; var dirs = 0; var links = 0
                for (line in man.readLines()) {
                    val p = line.split("\t")
                    when (p[0]) {
                        "D" -> {
                            val e = vol.stat("/" + p[1])
                            assertTrue("dir ${p[1]}", e.isDir)
                            assertEquals("mtime ${p[1]}", p[2].toLong(), e.mtimeSec)
                            vol.list("/" + p[1]); dirs++
                        }
                        "F" -> {
                            val path = "/" + p[1]
                            val size = p[2].toLong()
                            val e = vol.stat(path)
                            assertEquals("size ${p[1]}", size, e.size)
                            assertEquals("mtime ${p[1]}", p[3].toLong(), e.mtimeSec)
                            val regions = if (p.size > 5 && p[5].isNotEmpty()) p[5].split(",") else emptyList()
                            for (r in regions) {
                                val q = r.split(":")
                                if (q[0] == "d") checkRange(vol, path, q[1].toLong(), q[2].toLong(), q[3].toInt())
                                else {
                                    val off = q[1].toLong(); val len = q[2].toLong()
                                    val head = minOf(len, 8192L)
                                    checkRange(vol, path, off, head, -1)
                                    if (len > head) checkRange(vol, path, off + len - head, head, -1)
                                }
                            }
                            // EOF
                            assertEquals(0, vol.read(path, size, ByteArray(16)))
                            files++
                        }
                        "L" -> {
                            val target = p[2].toByteArray(Charsets.UTF_8)
                            val b = ByteArray(4096)
                            val n = vol.read("/" + p[1], 0, b)
                            assertEquals("link ${p[1]}", String(target, Charsets.UTF_8), String(b, 0, n, Charsets.UTF_8))
                            links++
                        }
                    }
                }
                assertTrue("files=$files", files >= 1000)
                assertEquals(3, links)
                assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                assertTrue(vol.list("/").any { it.name == "lost+found" })
                assertEquals(1000, vol.list("/many").size)
                assertEquals(5000, vol.list("/bigdir").size)   // htree ディレクトリ
                assertEquals(3, vol.list("/smalldir").size)
                android.util.Log.i("usbmem", "ext4 $name: $files files, $dirs dirs, $links links verified")
            }
        } finally { img.delete(); man.delete() }
    }

    /** マウントが理由付きで拒否される: needs_recovery → EUCLEAN(117)、暗号化 / casefold / verity → EOPNOTSUPP(95)。 */
    private fun refused(name: String, errno: Int) {
        val img = File(ctx.cacheDir, "ext4_$name.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                NtfsVolume.mountReadOnly(pfd.detachFd()).close()
                fail("$name must not be mounted")
            } catch (e: NativeException) {
                assertEquals("errno for $name", errno, e.code)
            }
        } finally { img.delete() }
    }

    @Test fun mountsAndVerifiesExt4Default4k() = verify("e4_def4k")            // extents / 64bit / flex_bg / metadata_csum / journal (クリーン) / htree / unwritten / 深い extent 木
    @Test fun mountsAndVerifiesExt4InlineData1k() = verify("e4_inline1k")      // inline_data (1 KiB ブロック)
    @Test fun mountsAndVerifiesExt4GdtCsum4k() = verify("e4_gdtcsum4k")        // uninit_bg (crc16)・メタデータのチェックサムなし
    @Test fun mountsAndVerifiesExt3Journal4k() = verify("e3_4k")               // ext3 (ブロックマップ + クリーンなジャーナル) も ext4 ドライバで読む
    @Test fun refusesNeedsRecovery() = refused("e4_needs_recovery", 117)
    @Test fun refusesEncrypted() = refused("rej_encrypt", 95)
    @Test fun refusesCasefold() = refused("rej_casefold", 95)
    @Test fun refusesVerity() = refused("rej_verity", 95)
}
