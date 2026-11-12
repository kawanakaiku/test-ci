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
 * 実 erofs イメージ (tools/gen_erofs_fixture.py: mkfs.erofs で作り、fsck.erofs で検査・展開し直した結果との一致を生成時に確認済み) を
 * Android 上で fs/erofs (LinuxCompat 上・読み取り専用) 経由で読む。
 * 事前に adb push で /data/local/tmp/erofs_<name>.{img,manifest,ref_1,ref_2} を置く (name = plain_4k / plain_1k / plain_noinline / plain_extended / chunk_blockmap / chunk_index / special_tar / lz4_4k / lz4hc_4k / lz4_big_pcluster / lz4_1k / lz4_tailpacking / lz4_fragments / lz4_dedupe / lzma_4k / deflate_4k / zstd_4k)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 */
class ErofsInstrumentedTest {
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
        val img = File(ctx.cacheDir, "erofs_$name.img")
        val man = File(ctx.cacheDir, "erofs_$name.manifest")
        val ref1 = File(ctx.cacheDir, "erofs_$name.ref_1")
        val ref2 = File(ctx.cacheDir, "erofs_$name.ref_2")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/erofs_$name.manifest", man) && shellCopy("/data/local/tmp/erofs_$name.img", img) &&
            shellCopy("/data/local/tmp/erofs_$name.ref_1", ref1))
        shellCopy("/data/local/tmp/erofs_$name.ref_2", ref2)   // special_tar には ref_2 が無い
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("erofs", vol.fsName)
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
                if (name == "special_tar") {
                    assertEquals(1, files); assertEquals(1, links); assertEquals(3, specials)
                } else {
                    assertTrue("files=$files", files >= 1000)
                    assertEquals(3, links)
                    assertEquals(2, specials)
                    assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                    assertEquals(1000, vol.list("/many").size)
                }
                android.util.Log.i("usbmem", "erofs $name: $files files, $dirs dirs, $links links, $specials specials verified")
            }
        } finally { img.delete(); man.delete(); ref1.delete(); ref2.delete() }
    }

    @Test fun mountsAndVerifiesPlain4k() = verify("plain_4k")
    @Test fun mountsAndVerifiesPlain1k() = verify("plain_1k")
    @Test fun mountsAndVerifiesNoInline() = verify("plain_noinline")
    @Test fun mountsAndVerifiesExtendedInodes() = verify("plain_extended")
    @Test fun mountsAndVerifiesChunkBlockmap() = verify("chunk_blockmap")
    @Test fun mountsAndVerifiesChunkIndex() = verify("chunk_index")
    @Test fun mountsAndVerifiesSpecialFiles() = verify("special_tar")

    // 圧縮 (z_erofs): lz4 / lz4hc / lzma / deflate / zstd。pcluster の大きさ・ブロックサイズ・tail packing・fragments・dedupe の組み合わせ
    @Test fun mountsAndVerifiesLz4() = verify("lz4_4k")
    @Test fun mountsAndVerifiesLz4hc() = verify("lz4hc_4k")
    @Test fun mountsAndVerifiesLz4BigPcluster() = verify("lz4_big_pcluster")
    @Test fun mountsAndVerifiesLz4Block1k() = verify("lz4_1k")
    @Test fun mountsAndVerifiesLz4TailPacking() = verify("lz4_tailpacking")
    @Test fun mountsAndVerifiesLz4Fragments() = verify("lz4_fragments")
    @Test fun mountsAndVerifiesLz4Dedupe() = verify("lz4_dedupe")
    @Test fun mountsAndVerifiesLzma() = verify("lzma_4k")
    @Test fun mountsAndVerifiesDeflate() = verify("deflate_4k")
    @Test fun mountsAndVerifiesZstd() = verify("zstd_4k")

    /** 壊れたイメージ (マジックを壊す) は、クラッシュせずマウントが失敗する。 */
    @Test fun corruptMagicIsRefused() {
        val img = File(ctx.cacheDir, "erofs_corrupt.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/erofs_lz4_4k.img", img))
        try {
            java.io.RandomAccessFile(img, "rw").use { it.seek(1024); it.write(byteArrayOf(0, 0, 0, 0)) }
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                NtfsVolume.mountReadOnly(pfd.detachFd()).close()
                fail("corrupt erofs should be refused")
            } catch (e: NativeException) {
                assertTrue(e.code != 0)
            }
        } finally { img.delete() }
    }
}
