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
 * Btrfs の書き込み (実験的。-PusbmemNtfsWrite=true のビルドだけ) を Android 上で自前のエンジン (core/compat/src/btrfs_write*.inc) 経由で行う。
 * tools/run_android_btrfs_write_test.sh から実行する。ホストが /data/local/tmp に置くもの:
 *   btrfs_wr.img        書き込み対象のイメージ (mkfs.btrfs --rootdir で元ファイル入り。tools/gen_btrfs_ops.py seed)
 *   btrfs_wr_ops.txt    操作列 (tools/gen_btrfs_ops.py ops ... seeded。usbmem_btrfs_write_tests --ops / tools/ops_ref.py と同じ書式)
 *   btrfs_sv.img        サブボリュームを含むイメージ (書き込み拒否 = 読み取り専用に落ちること)
 * 結果イメージは cache/btrfs_rw_result.img に残す。ホストが pull して btrfs check と btrfs restore で独立に検証する。
 */
class BtrfsWriteTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun mountRw(img: File): NtfsVolume {
        val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE)
        try {
            return NtfsVolume.mountReadWrite(pfd.detachFd(), createOnly = false)
        } catch (ex: NativeException) {
            assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30)
            throw ex
        }
    }

    private fun split(p: String): Pair<String, String> {
        val i = p.lastIndexOf('/')
        return Pair(if (i <= 0) "/" else p.substring(0, i), p.substring(i + 1))
    }

    /** ops_ref.py と同じパターン: ファイル先頭からの位置 p のバイトが (p*7 + seed) & 255。 */
    private fun writePattern(vol: NtfsVolume, path: String, off: Long, len: Long, seed: Int) {
        var pos = off
        val end = off + len
        while (pos < end) {
            val n = minOf(1L shl 20, end - pos).toInt()
            val b = ByteArray(n) { (((pos + it) * 7 + seed) and 255).toByte() }
            val w = vol.write(path, pos, b)
            if (w != n) throw AssertionError("short write $path @$pos: $w/$n")
            pos += n
        }
    }

    private fun runOp(vol: NtfsVolume, line: String) {
        val f = line.trim().split(Regex("\\s+"))
        if (f.isEmpty() || f[0].isEmpty() || f[0].startsWith("#")) return
        when (f[0]) {
            "mkdir" -> split(f[1]).let { vol.create(it.first, it.second, true) }
            "create" -> split(f[1]).let { vol.create(it.first, it.second, false) }
            "unlink", "rmdir" -> vol.remove(f[1])
            "rename" -> split(f[2]).let { vol.move(f[1], it.first, it.second) }
            "truncate" -> vol.truncate(f[1], f[2].toLong())
            "symlink" -> split(f[1]).let { vol.symlink(it.first, it.second, f[2]) }
            "link" -> split(f[1]).let { vol.link(it.first, it.second, f[2]) }
            "write" -> writePattern(vol, f[1], f[2].toLong(), f[3].toLong(), f[4].toInt())
            else -> throw AssertionError("bad op: $line")
        }
    }

    @Test
    fun scriptedOperationsLeaveCleanVolume() {
        val img = File(ctx.cacheDir, "btrfs_wr.img")
        val ops = File(ctx.cacheDir, "btrfs_wr_ops.txt")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/btrfs_wr_ops.txt", ops) && shellCopy("/data/local/tmp/btrfs_wr.img", img))
        val result = File(ctx.cacheDir, "btrfs_rw_result.img")
        result.delete()
        val vol = mountRw(img)
        var n = 0
        try {
            assertEquals("btrfs", vol.fsName)
            assertTrue("btrfs must mount writable", vol.writable)
            for (line in ops.readLines()) {
                try { runOp(vol, line) } catch (ex: Throwable) { throw AssertionError("op #$n failed: $line", ex) }
                n++
            }
            vol.sync()
            // 書いた内容を同じセッション内でも読み戻す (パターン)
            val e = vol.stat("/w/big.bin")
            assertEquals(5_304_999L, e.size)
            val buf = ByteArray(4096)
            assertEquals(4096, vol.read("/w/big.bin", 200_000, buf))
            for (i in buf.indices) assertEquals("big.bin @${200_000 + i}", (((200_000 + i).toLong() * 7 + 6) and 255).toInt(), buf[i].toInt() and 255)
            assertEquals("/etc/passwd", vol.readlink("/w/short.lnk"))
        } finally { vol.close() }
        assertTrue(n > 100)
        img.copyTo(result, overwrite = true)   // 閉じたあとのイメージ (コミット済み)
        img.delete()
        android.util.Log.i("usbmem", "btrfs write test: $n ops done")
    }

    /** サブボリュームを含むイメージは書き込み拒否 (読み取り専用で開ける。読み取りはでき、書き込みは EROFS=30、イメージは 1 バイトも変わらない)。 */
    @Test
    fun unsupportedVolumeFallsBackToReadOnly() {
        val img = File(ctx.cacheDir, "btrfs_sv.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/btrfs_sv.img", img))
        val before = img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() }
        val vol = mountRw(img)
        try {
            assertEquals("btrfs", vol.fsName)
            assertFalse("a volume with subvolumes must not be writable", vol.writable)
            try { vol.create("/", "x", false); throw AssertionError("create must fail on a read-only volume") } catch (ex: NativeException) { assertEquals(30, ex.code) }
            vol.list("/")   // 読み取りはできる
        } finally { vol.close() }
        assertEquals(before, img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() })
        img.delete()
    }

    /** 書き込み可でマウントしても、書かずに閉じればイメージは 1 バイトも変わらない。 */
    @Test
    fun rwMountWithoutWritesLeavesImageUnchanged() {
        val img = File(ctx.cacheDir, "btrfs_noop.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/btrfs_wr.img", img))
        val before = img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() }
        val vol = mountRw(img)
        try {
            assertEquals("btrfs", vol.fsName)
            assertTrue(vol.writable)
            vol.list("/")
        } finally { vol.close() }
        assertEquals(before, img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() })
        img.delete()
    }
}
