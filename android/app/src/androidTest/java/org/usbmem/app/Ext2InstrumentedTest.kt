package org.usbmem.app

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream

/**
 * 実 ext2 イメージ (tools/gen_ext2_fixture.py) を Android 上で fs/ext2 経由で読む (読み取り専用)。
 * 事前に adb push で /data/local/tmp/ext2_<name>.{img,manifest} を置く (name = b1k / b2k / b4k。ext3_empty / ext4_empty は .img のみ = ext4 ドライバでマウントされることの確認用)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF (i はファイル先頭からのオフセット)。
 */
class Ext2InstrumentedTest {
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
        val img = File(ctx.cacheDir, "ext2_$name.img")
        val man = File(ctx.cacheDir, "ext2_$name.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext2_$name.manifest", man) && shellCopy("/data/local/tmp/ext2_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("ext2", vol.fsName)
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
                android.util.Log.i("usbmem", "ext2 $name: $files files, $dirs dirs, $links links verified")
            }
        } finally { img.delete(); man.delete() }
    }

    /** ext3 / ext4 の空イメージは fs/ext2 ではなく fs/ext4 (読み取り専用) でマウントされる (NTFS として試されることもない)。 */
    private fun mountedByExt4(name: String) {
        val img = File(ctx.cacheDir, "ext2_$name.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext2_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("ext4", vol.fsName)
                assertFalse(vol.writable)
                assertTrue(vol.list("/").any { it.name == "lost+found" })
            }
        } finally { img.delete() }
    }

    @Test fun mountsAndVerifiesB1k() = verify("b1k")
    @Test fun mountsAndVerifiesB2k() = verify("b2k")
    @Test fun mountsAndVerifiesB4k() = verify("b4k")
    @Test fun ext3IsMountedByExt4Driver() = mountedByExt4("ext3_empty")
    @Test fun ext4IsMountedByExt4Driver() = mountedByExt4("ext4_empty")
}
