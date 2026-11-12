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
 * 実 F2FS イメージを Android 上で自前の読み取り専用リーダー (F2fsVolume) 経由で読む。イメージは tools/gen_f2fs_fixture.py (mkfs.f2fs + sload.f2fs。fsck.f2fs で整合性を確認) と
 * tools/f2fs_vm/gen_k_fixtures.py (QEMU 上の本物の Linux カーネルが書いたもの: 圧縮 lz4 / lzo / lzo-rle / zstd、深いノード木、roll-forward が必要な状態) の出力。
 * 事前に tools/run_android_f2fs_test.sh が /data/local/tmp/f2fs_<name>.{img.gz,manifest,ref_1,ref_2} を置く (.img.gz はゼロ領域の多いイメージの gzip。端末側で疎ファイルに展開する)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 */
class F2fsInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    /** /data/local/tmp/f2fs_<name>.img.gz を cacheDir の <name>.img へ展開する (ゼロだけの 64 KiB は書かずに飛ばして疎ファイルにする)。 */
    private fun inflateImage(name: String, dst: File): Boolean {
        val gz = File(ctx.cacheDir, "f2fs_$name.img.gz")
        if (!shellCopy("/data/local/tmp/f2fs_$name.img.gz", gz)) return false
        try {
            java.util.zip.GZIPInputStream(gz.inputStream().buffered(1 shl 20)).use { ins ->
                java.io.RandomAccessFile(dst, "rw").use { raf ->
                    val buf = ByteArray(65536)
                    var pos = 0L
                    while (true) {
                        var n = 0
                        while (n < buf.size) { val r = ins.read(buf, n, buf.size - n); if (r <= 0) break; n += r }
                        if (n == 0) break
                        var zero = true
                        for (i in 0 until n) if (buf[i].toInt() != 0) { zero = false; break }
                        if (!zero) { raf.seek(pos); raf.write(buf, 0, n) }
                        pos += n
                        if (n < buf.size) break
                    }
                    raf.setLength(pos)
                }
            }
        } finally { gz.delete() }
        return true
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
        val img = File(ctx.cacheDir, "f2fs_$name.img")
        val man = File(ctx.cacheDir, "f2fs_$name.manifest")
        val ref1 = File(ctx.cacheDir, "f2fs_$name.ref_1")
        val ref2 = File(ctx.cacheDir, "f2fs_$name.ref_2")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/f2fs_$name.manifest", man) && inflateImage(name, img) &&
            shellCopy("/data/local/tmp/f2fs_$name.ref_1", ref1) && shellCopy("/data/local/tmp/f2fs_$name.ref_2", ref2))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("f2fs", vol.fsName)
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
                assertEquals(if (name.startsWith("k_")) 4 else 0, specials)   // mkfs/sload のイメージには特殊ファイルを入れられない
                assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                assertEquals(1000, vol.list("/many").size)
                android.util.Log.i("usbmem", "f2fs $name: $files files, $dirs dirs, $links links, $specials specials verified")
            }
        } finally { img.delete(); man.delete(); ref1.delete(); ref2.delete() }
    }

    @Test fun mountsAndVerifiesMkfsSload() = verify("ext")
    @Test fun mountsAndVerifiesKernelDeepTree() = verify("k_basic")
    @Test fun mountsAndVerifiesKernelNoFeatures() = verify("k_nofeat")
    @Test fun mountsAndVerifiesKernelDirty() = verify("k_dirty")
    @Test fun mountsAndVerifiesLz4Cluster4() = verify("k_comp_lz4_c2")
    @Test fun mountsAndVerifiesLz4Cluster16() = verify("k_comp_lz4_c4")
    @Test fun mountsAndVerifiesLzo() = verify("k_comp_lzo_c2")
    @Test fun mountsAndVerifiesLzoRle() = verify("k_comp_lzorle_c3")
    @Test fun mountsAndVerifiesZstd() = verify("k_comp_zstd_c2")

    /** チェックポイントのあとに fsync だけされて切られたボリューム (roll-forward が必要) は EUCLEAN(117) で拒否され、確認のうえなら「回復を無視して」読み取り専用で開ける。 */
    @Test fun rollForwardIsRefusedUnlessNoload() {
        val img = File(ctx.cacheDir, "f2fs_k_unclean.img")
        assumeTrue("fixture not pushed", inflateImage("k_unclean", img))
        try {
            var pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                NtfsVolume.mountReadOnly(pfd.detachFd()).close()
                fail("unclean f2fs should be refused")
            } catch (e: NativeException) {
                assertEquals(117, e.code)
            }
            pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd(), allowNoload = true).use { vol ->
                assertEquals("f2fs", vol.fsName)
                assertFalse(vol.writable)
                assertTrue(vol.journalIgnored)
                assertTrue(vol.list("/").any { it.name == "small.txt" })
                assertEquals(11L, vol.stat("/small.txt").size)   // fsync の分 (追記) は見えず、チェックポイントの内容が見える
            }
        } finally { img.delete() }
    }
}
