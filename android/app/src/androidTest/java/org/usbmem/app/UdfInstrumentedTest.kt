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
 * 実 ISO 9660 イメージ (tools/gen_udf_fixture.py) を Android 上で fs/isofs 経由で読む (読み取り専用)。
 * 事前に adb push で /data/local/tmp/udf_<name>.{img,manifest} を置く (name = hd512 / hd2048 / spar150 / bridge / vat250_empty)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec mode / L path target / V key value (疎ファイルの spec はカンマ区切りで複数領域)。
 */
class UdfInstrumentedTest {
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
                val i = pos + k
                val w = if (seed >= 0) want(seed, i) else 0
                val g = buf[k].toInt() and 0xFF
                if (g != w) throw AssertionError("$path @$i: $g != $w")
            }
            pos += n
        }
    }

    private fun verify(name: String, minFiles: Int, minLinks: Int) {
        val img = File(ctx.cacheDir, "udf_$name.img")
        val man = File(ctx.cacheDir, "udf_$name.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/udf_$name.manifest", man) && shellCopy("/data/local/tmp/udf_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("udf", vol.fsName)
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
                            val spec = if (p.size > 5) p[5] else ""
                            for (r in spec.split(",").filter { it.isNotEmpty() }) {
                                val q = r.split(":")
                                if (q[0] == "d") checkRange(vol, path, q[1].toLong(), q[2].toLong(), q[3].toInt())
                            }
                            assertEquals(0, vol.read(path, size, ByteArray(16)))   // EOF
                            files++
                        }
                        "L" -> {
                            val b = ByteArray(4096)
                            val n = vol.read("/" + p[1], 0, b)
                            assertEquals("link ${p[1]}", p[2], String(b, 0, n, Charsets.UTF_8))
                            links++
                        }
                    }
                }
                assertTrue("files=$files", files >= minFiles)
                assertTrue("links=$links", links >= minLinks)
                android.util.Log.i("usbmem", "udf $name: $files files, $dirs dirs, $links links verified")
            }
        } finally { img.delete(); man.delete() }
    }

    @Test fun blocksize512() = verify("hd512", 1000, 6)
    @Test fun blocksize2048() {
        verify("hd2048", 1000, 7)
        val img = File(ctx.cacheDir, "udf_chk.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/udf_hd2048.img", img))
        try {
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).use { vol ->
                val names = vol.list("/").map { it.name }
                assertTrue(names.contains("日本語ファイル名.txt"))
                assertTrue(names.contains("n".repeat(200)))
                assertEquals(1000, vol.list("/many").size)
                assertTrue(vol.stat("/link_rel").isLink)
                // ハードリンク: 同じ ICB (2 つの名前で同じ inode)
                assertEquals(vol.stat("/hl_orig.bin").ino, vol.stat("/hl_copy.bin").ino)
            }
        } finally { img.delete() }
    }
    @Test fun sparable() = verify("spar150", 1000, 7)
    /** ISO 9660 + UDF 1.02 のブリッジ: UDF を優先してマウントする。 */
    @Test fun bridgePrefersUdf() = verify("bridge", 1000, 0)
    /** UDF 2.50 の VAT (仮想パーティション、追記型メディア) の空ボリューム。 */
    @Test fun virtualPartitionEmpty() {
        val img = File(ctx.cacheDir, "udf_vat.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/udf_vat250_empty.img", img))
        try {
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).use { vol ->
                assertEquals("udf", vol.fsName)
                assertEquals(0, vol.list("/").size)
            }
        } finally { img.delete() }
    }

    /** UDF ではないイメージ (全ゼロ) はマウントされない。 */
    @Test fun notIsoIsRefused() {
        val img = File(ctx.cacheDir, "udf_zero.img")
        img.outputStream().use { it.write(ByteArray(1 shl 20)) }
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try { NtfsVolume.mountReadOnly(pfd.detachFd()).close(); fail("must not mount") } catch (e: NativeException) { /* 期待どおり */ }
        } finally { img.delete() }
    }
}
