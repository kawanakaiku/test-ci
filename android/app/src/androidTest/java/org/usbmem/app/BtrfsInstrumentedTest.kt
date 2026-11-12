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
import java.io.RandomAccessFile

/**
 * 実 Btrfs イメージ (tools/gen_btrfs_fixture.py: mkfs.btrfs で作り、btrfs check --readonly と btrfs restore との一致を生成時に確認済み) を
 * Android 上の自前リーダー (読み取り専用) で読む。事前に端末へ /data/local/tmp/btrfs_<name>.{img,manifest,ref_1,ref_2} を置く (tools/run_android_btrfs_test.sh)。
 * manifest はタブ区切り: D path mtime / F path size mtime nlink spec / L path target / S path kind / R path size mtime reffile。
 * spec = "d:<off>:<len>:<seed>" (パターン内容) と "z:<off>:<len>" (ゼロ) のカンマ区切り。パターンは byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 */
class BtrfsInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun want(seed: Int, i: Long): Int = ((i * 31 + seed * 7 + (i shr 8)) and 0xFF).toInt()

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

    private fun fetch(name: String, exts: List<String>): Map<String, File>? {
        val files = HashMap<String, File>()
        for (e in exts) {
            val f = File(ctx.cacheDir, "btrfs_$name.$e")
            if (shellCopy("/data/local/tmp/btrfs_$name.$e", f)) files[e] = f else f.delete()
        }
        return if (files.containsKey("img")) files else null
    }

    private fun verify(name: String) {
        val fx = fetch(name, listOf("manifest", "ref_1", "ref_2", "img"))
        assumeTrue("fixture not pushed", fx != null && fx.containsKey("manifest"))
        val img = fx!!["img"]!!; val man = fx["manifest"]!!
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("btrfs", vol.fsName)
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
                            val ref = (if (p[4].endsWith("ref_1")) fx["ref_1"] else fx["ref_2"])!!
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
                        "S" -> {   // 特殊ファイル (FIFO / ソケット): 一覧に出て、読み取りは失敗する
                            assertTrue("special ${p[1]}", vol.list("/").any { it.name == p[1] })
                            try { vol.read("/" + p[1], 0, ByteArray(8)); fail("read of special file ${p[1]} should fail") } catch (_: NativeException) {}
                            specials++
                        }
                    }
                }
                assertTrue("links=$links", links >= 1)
                assertTrue("specials=$specials", specials >= 1)
                if (name.startsWith("sector")) {
                    assertTrue("files=$files", files >= 19)
                } else {
                    assertTrue("files=$files", files >= 1000)
                    assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                    assertEquals(1000, vol.list("/many").size)
                }
                android.util.Log.i("usbmem", "btrfs $name: $files files, $dirs dirs, $links links, $specials specials verified")
            }
        } finally { fx.values.forEach { it.delete() } }
    }

    // チェックサム × 圧縮 × ノードサイズ × セクタサイズ × 機能
    @Test fun mountsAndVerifiesCrc32c() = verify("crc32c")
    @Test fun mountsAndVerifiesXxhash() = verify("xxhash")
    @Test fun mountsAndVerifiesSha256() = verify("sha256")
    @Test fun mountsAndVerifiesBlake2() = verify("blake2")
    @Test fun mountsAndVerifiesZlib() = verify("zlib")
    @Test fun mountsAndVerifiesLzo() = verify("lzo")
    @Test fun mountsAndVerifiesZstd() = verify("zstd")
    @Test fun mountsAndVerifiesNode4k() = verify("node4k")
    @Test fun mountsAndVerifiesNode64k() = verify("node64k")
    @Test fun mountsAndVerifiesSector16kLzo() = verify("sector16k_lzo")
    @Test fun mountsAndVerifiesSector64kZlib() = verify("sector64k_zlib")
    @Test fun mountsAndVerifiesDupDup() = verify("dup_dup")
    @Test fun mountsAndVerifiesMixed() = verify("mixed")
    @Test fun mountsAndVerifiesNoHoles() = verify("noholes")

    /** サブボリューム: 親のサブボリュームから子 (ネスト含む) が普通のディレクトリのように辿れる。 */
    @Test fun subvolumesAreTraversable() {
        val fx = fetch("subvols", listOf("img"))
        assumeTrue("fixture not pushed", fx != null)
        val img = fx!!["img"]!!
        try {
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).use { vol ->
                assertEquals("btrfs", vol.fsName)
                assertEquals(setOf("top.txt", "svA", "svB", "svdef"), vol.list("/").map { it.name }.toSet())
                assertTrue(vol.stat("/svA").isDir)
                assertEquals(setOf("a.txt", "inner"), vol.list("/svA").map { it.name }.toSet())
                assertEquals("nested subvolume\n", String(readAll(vol, "/svA/inner/i.txt", vol.stat("/svA/inner/i.txt").size)))
                assertEquals("in svB (read-only)\n", String(readAll(vol, "/svB/b.txt", vol.stat("/svB/b.txt").size)))
                // サブボリュームの境界をまたぐ読み取りでも書き込みは拒否
                try { vol.create("/svA", "new.txt", false); fail("btrfs must stay read-only") } catch (_: NativeException) {}
            }
        } finally { fx.values.forEach { it.delete() } }
    }

    /** 複数デバイスの RAID1 (1 台目だけ) は EOPNOTSUPP(95) で拒否される。 */
    @Test fun multiDeviceIsRefused() {
        val fx = fetch("multi_a", listOf("img"))
        assumeTrue("fixture not pushed", fx != null)
        val img = fx!!["img"]!!
        try {
            try {
                NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).close()
                fail("multi-device btrfs must be refused")
            } catch (e: NativeException) { assertEquals(95, e.code) }
        } finally { fx.values.forEach { it.delete() } }
    }

    // ---- tree-log が未再生 (log_root != 0): 既定は EUCLEAN(117)、確認のうえ allowNoload なら読み取り専用で開ける。イメージは1バイトも変わらない
    private fun crc32c(b: ByteArray, off: Int, len: Int): Int {
        var c = -1
        for (i in off until off + len) {
            c = c xor (b[i].toInt() and 0xFF)
            for (k in 0 until 8) c = if (c and 1 != 0) (c ushr 1) xor 0x82F63B78.toInt() else c ushr 1
        }
        return c.inv()
    }

    @Test fun unreplayedLogIsRefusedUnlessAllowedAndStaysReadOnly() {
        val fx = fetch("crc32c", listOf("img"))
        assumeTrue("fixture not pushed", fx != null)
        val img = fx!!["img"]!!
        try {
            RandomAccessFile(img, "rw").use { f ->
                val sb = ByteArray(4096)
                f.seek(0x10000); f.readFully(sb)
                sb[0x62] = 0x10   // log_root = 0x100000
                val c = crc32c(sb, 32, 4096 - 32)
                for (i in 0 until 4) sb[i] = (c ushr (8 * i)).toByte()
                f.seek(0x10000); f.write(sb)
            }
            val before = img.readBytes().contentHashCode()
            try { NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).close(); fail("must be refused by default") }
            catch (e: NativeException) { assertEquals(117, e.code) }
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd(), allowNoload = true).use { vol ->
                assertEquals("btrfs", vol.fsName)
                assertTrue(vol.journalIgnored)
                assertFalse(vol.writable)
                assertTrue(vol.list("/").isNotEmpty())
                try { vol.create("/", "x", false); fail("write must be refused") } catch (_: NativeException) {}
            }
            assertEquals("image must be unchanged", before, img.readBytes().contentHashCode())
        } finally { fx.values.forEach { it.delete() } }
    }

    /** 壊れたイメージ (スーパーブロックのマジック) は、クラッシュせずマウントが失敗する。 */
    @Test fun corruptMagicIsRefused() {
        val fx = fetch("crc32c", listOf("img"))
        assumeTrue("fixture not pushed", fx != null)
        val img = fx!!["img"]!!
        try {
            RandomAccessFile(img, "rw").use { f ->
                for (off in longArrayOf(0x10040L, 0x4000040L)) { f.seek(off); f.write(byteArrayOf(0, 0, 0, 0)) }   // 主と 64 MiB のミラーの両方
            }
            try {
                NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).close()
                fail("corrupt btrfs should be refused")
            } catch (e: NativeException) { assertTrue(e.code != 0) }
        } finally { fx.values.forEach { it.delete() } }
    }
}
