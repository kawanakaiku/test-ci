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
 * 実 XFS イメージ (tools/gen_xfs_fixture.py: mkfs.xfs -p <dir> で作り、xfs_repair -n で検査済み) を Android 上で XfsVolume (独立実装の読み取り専用リーダ) 経由で読む。
 * 事前に adb push で /data/local/tmp/xfs_<name>.{img,manifest} (+ special_proto は ref_1) を置く (name = v5_default / v4_ftype0 / v5_bs1k / v5_nrext64 / special_proto)。
 * イメージは mkfs.xfs の最小 (300 MiB) なので、/data/local/tmp から直接開けるときはコピーせずそのまま開く。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 */
class XfsInstrumentedTest {
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

    /** イメージを開くファイル。/data/local/tmp から直接読めるならそれ (コピーしない)。読めなければアプリのキャッシュへコピー。 */
    private fun openable(name: String, copies: MutableList<File>): File? {
        val direct = File("/data/local/tmp/xfs_$name.img")
        if (direct.canRead()) return direct
        val img = File(ctx.cacheDir, "xfs_$name.img")
        if (!shellCopy("/data/local/tmp/xfs_$name.img", img)) return null
        copies.add(img)
        return img
    }

    private fun verify(name: String) {
        val copies = mutableListOf<File>()
        val man = File(ctx.cacheDir, "xfs_$name.manifest")
        val ref1 = File(ctx.cacheDir, "xfs_$name.ref_1")
        val ref2 = File(ctx.cacheDir, "xfs_$name.ref_2")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_$name.manifest", man))
        val img = openable(name, copies)
        assumeTrue("image not readable", img != null)
        shellCopy("/data/local/tmp/xfs_$name.ref_1", ref1)
        shellCopy("/data/local/tmp/xfs_$name.ref_2", ref2)   // special_proto には ref_2 が無い
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("xfs", vol.fsName)
                assertFalse(vol.writable)
                assertFalse(vol.journalIgnored)
                var files = 0; var dirs = 0; var links = 0; var specials = 0
                for (line in man.readLines()) {
                    val p = line.split("\t")
                    when (p[0]) {
                        "D" -> {
                            val e = vol.stat("/" + p[1])
                            assertTrue("dir ${p[1]}", e.isDir)
                            if (p[2].toLong() >= 0) assertEquals("mtime ${p[1]}", p[2].toLong(), e.mtimeSec)
                            vol.list("/" + p[1]); dirs++
                        }
                        "F" -> {
                            val path = "/" + p[1]
                            val size = p[2].toLong()
                            val e = vol.stat(path)
                            assertEquals("size ${p[1]}", size, e.size)
                            if (p[3].toLong() >= 0) assertEquals("mtime ${p[1]}", p[3].toLong(), e.mtimeSec)
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
                            if (p[3].toLong() >= 0) assertEquals("mtime ${p[1]}", p[3].toLong(), e.mtimeSec)
                            assertTrue("ref content ${p[1]}", (if (p[4].endsWith("ref_1")) ref1 else ref2).readBytes().contentEquals(readAll(vol, path, size)))
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
                if (name == "special_proto") {
                    assertEquals(1, files); assertEquals(1, links); assertEquals(3, specials)
                } else {
                    assertTrue("files=$files", files >= 1000)
                    assertTrue("links=$links", links >= 3)
                    assertEquals(2, specials)
                    assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                    assertEquals(1000, vol.list("/many").size)
                }
                android.util.Log.i("usbmem", "xfs $name: $files files, $dirs dirs, $links links, $specials specials verified")
            }
        } finally { man.delete(); ref1.delete(); ref2.delete(); copies.forEach { it.delete() } }
    }

    @Test fun mountsAndVerifiesV5Default() = verify("v5_default")      // v5 (CRC)・4 KiB・ftype・sparse inode・bigtime・nrext64・reflink・rmapbt・finobt、sf/block/leaf/node/btree ディレクトリ、btree extent、リモートシンボリックリンク
    @Test fun mountsAndVerifiesV4NoFtype() = verify("v4_ftype0")      // v4 (CRC なし)・ftype なし (種別は inode から)
    @Test fun mountsAndVerifiesBlock1kDir4k() = verify("v5_bs1k")      // 1 KiB ブロック + 4 KiB ディレクトリブロック (ディレクトリブロックが複数の fs ブロック)
    @Test fun mountsAndVerifiesNrext64() = verify("v5_nrext64")
    @Test fun mountsAndVerifiesSpecialFiles() = verify("special_proto")

    private fun be32(b: ByteArray, o: Int): Long = ((b[o].toLong() and 0xFF) shl 24) or ((b[o + 1].toLong() and 0xFF) shl 16) or ((b[o + 2].toLong() and 0xFF) shl 8) or (b[o + 3].toLong() and 0xFF)

    /** ログが未再生に見える (クリーンなアンマウントでない) イメージは、既定では EUCLEAN(117) で拒否され、確認のうえ (allowNoload) なら読み取り専用で開ける。書き込みは一切できず、イメージは変わらない。 */
    @Test fun dirtyLogIsRefusedUnlessAllowedAndNeverWritable() {
        val img = File(ctx.cacheDir, "xfs_dirty.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_v5_default.img", img))
        try {
            java.io.RandomAccessFile(img, "rw").use { f ->
                val sb = ByteArray(512); f.readFully(sb)
                val bs = be32(sb, 4); val agblocks = be32(sb, 84); val agblklog = sb[124].toInt()
                var logstart = 0L; for (i in 0 until 8) logstart = (logstart shl 8) or (sb[48 + i].toLong() and 0xFF)
                val agno = logstart shr agblklog; val agbno = logstart and ((1L shl agblklog) - 1)
                val logoff = (agno * agblocks + agbno) * bs
                f.seek(logoff + 512 + 9); f.write(0)      // アンマウントレコードのフラグを消す → 未再生の更新があるように見える
            }
            val before = img.length()
            try { NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).close(); fail("dirty log must be refused by default") }
            catch (e: NativeException) { assertEquals(117, e.code) }
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd(), allowNoload = true).use { vol ->
                assertEquals("xfs", vol.fsName)
                assertTrue(vol.journalIgnored)
                assertFalse(vol.writable)
                assertTrue(vol.list("/").isNotEmpty())
                try { vol.create("/", "x", false); fail("write must be refused") } catch (e: NativeException) { assertEquals(30, e.code) }
            }
            assertEquals("image size must be unchanged", before, img.length())
        } finally { img.delete() }
    }

    /** 壊れたイメージ (マジックを壊す) は、クラッシュせずマウントが失敗する。 */
    @Test fun corruptMagicIsRefused() {
        val img = File(ctx.cacheDir, "xfs_corrupt.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_v4_ftype0.img", img))
        try {
            java.io.RandomAccessFile(img, "rw").use { it.seek(0); it.write(byteArrayOf(0, 0, 0, 0)) }
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            try {
                NtfsVolume.mountReadOnly(pfd.detachFd()).close()
                fail("corrupt xfs should be refused")
            } catch (e: NativeException) {
                assertTrue(e.code != 0)
            }
        } finally { img.delete() }
    }
}
