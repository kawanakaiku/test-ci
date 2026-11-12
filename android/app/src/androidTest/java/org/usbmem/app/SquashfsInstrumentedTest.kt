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
 * 実 squashfs イメージ (tools/gen_squashfs_fixture.py: mksquashfs で作り、unsquashfs で展開し直した結果との一致を生成時に確認済み) を
 * Android 上で fs/squashfs (LinuxCompat 上・読み取り専用) 経由で読む。
 * 事前に adb push で /data/local/tmp/squashfs_<name>.{img,manifest,ref_1,ref_2} を置く (name = gzip_128k / xz_128k / zstd_128k / lz4_128k / lzo_128k / gzip_4k / nocomp)。
 * lzma_refused.img は .img のみ (LZMA1 圧縮 = 非対応のためマウントが拒否されること)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 */
class SquashfsInstrumentedTest {
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
            for (k in 0 until n) {
                val w = if (seed >= 0) want(seed, pos + k) else 0
                if ((buf[k].toInt() and 0xFF) != w) throw AssertionError("$path @${pos + k}: ${buf[k].toInt() and 0xFF} != $w")
            }
            pos += n
        }
    }

    private fun readAll(vol: NtfsVolume, path: String, size: Long): ByteArray {
        val out = ByteArray(size.toInt())
        var pos = 0
        while (pos < out.size) {
            val b = ByteArray(minOf(65536, out.size - pos))
            val n = vol.read(path, pos.toLong(), b)
            if (n <= 0) throw AssertionError("$path @$pos: short read")
            System.arraycopy(b, 0, out, pos, n); pos += n
        }
        return out
    }

    private fun verify(name: String) {
        val img = File(ctx.cacheDir, "squashfs_$name.img")
        val man = File(ctx.cacheDir, "squashfs_$name.manifest")
        val ref1 = File(ctx.cacheDir, "squashfs_$name.ref_1")
        val ref2 = File(ctx.cacheDir, "squashfs_$name.ref_2")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/squashfs_$name.manifest", man) && shellCopy("/data/local/tmp/squashfs_$name.img", img) &&
            shellCopy("/data/local/tmp/squashfs_$name.ref_1", ref1) && shellCopy("/data/local/tmp/squashfs_$name.ref_2", ref2))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("squashfs", vol.fsName)
                assertFalse(vol.writable)
                var files = 0; var dirs = 0; var links = 0; var specials = 0
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
                            assertEquals(0, vol.read(path, size, ByteArray(16)))   // EOF
                            files++
                        }
                        "R" -> {
                            val path = "/" + p[1]
                            val size = p[2].toLong()
                            val e = vol.stat(path)
                            assertEquals("size ${p[1]}", size, e.size)
                            assertEquals("mtime ${p[1]}", p[3].toLong(), e.mtimeSec)
                            val ref = if (p[4].endsWith("ref_1")) ref1 else ref2
                            assertTrue("ref content ${p[1]}", ref.readBytes().contentEquals(readAll(vol, path, size)))
                            files++
                        }
                        "L" -> {
                            val b = ByteArray(4096)
                            val n = vol.read("/" + p[1], 0, b)
                            assertEquals("link ${p[1]}", p[2], String(b, 0, n, Charsets.UTF_8))
                            assertEquals("readlink ${p[1]}", p[2], vol.readlink("/" + p[1]))
                            assertTrue(vol.stat("/" + p[1]).isLink)
                            links++
                        }
                        "S" -> {   // 特殊ファイル (キャラクタ / ブロック / FIFO / ソケット): 一覧に出て、読み取りは失敗する
                            assertTrue("special ${p[1]}", vol.list("/").any { it.name == p[1] })
                            try { vol.read("/" + p[1], 0, ByteArray(8)); fail("read of special file ${p[1]} should fail") } catch (_: NativeException) {}
                            specials++
                        }
                    }
                }
                assertTrue("files=$files", files >= 1000)
                assertEquals(3, links)
                assertEquals(4, specials)
                assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                assertEquals(1000, vol.list("/many").size)
                android.util.Log.i("usbmem", "squashfs $name: $files files, $dirs dirs, $links links, $specials specials verified")
            }
        } finally { img.delete(); man.delete(); ref1.delete(); ref2.delete() }
    }

    @Test fun mountsAndVerifiesGzip128k() = verify("gzip_128k")
    @Test fun mountsAndVerifiesGzip4k() = verify("gzip_4k")
    @Test fun mountsAndVerifiesXz() = verify("xz_128k")
    @Test fun mountsAndVerifiesZstd() = verify("zstd_128k")
    @Test fun mountsAndVerifiesLz4() = verify("lz4_128k")
    @Test fun mountsAndVerifiesLzo() = verify("lzo_128k")
    @Test fun mountsAndVerifiesUncompressed() = verify("nocomp")

    /** LZMA1 圧縮 (非対応) は EOPNOTSUPP(95) でマウントが拒否される。 */
    @Test fun lzmaIsRefused() {
        val img = File(ctx.cacheDir, "squashfs_lzma_refused.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/squashfs_lzma_refused.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                NtfsVolume.mountReadOnly(pfd.detachFd()).close()
                fail("lzma squashfs should be refused")
            } catch (e: NativeException) {
                assertEquals(95, e.code)
            }
        } finally { img.delete() }
    }
}
