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
 * exFAT の書き込み (実験的。-PusbmemNtfsWrite=true のビルドだけ) を Android 上で fs/exfat 経由で行う。tools/run_android_exfat_write_test.sh から実行する。
 * ホストが /data/local/tmp に置くもの:
 *   exfat_wr.img        書き込み対象のフィクスチャ (tools/gen_exfat_fixture.py の c4k)
 *   exfat_wr_ops.txt    操作列 (tools/gen_exfat_ops.py。usbmem_exfat_write_tests --ops / tools/ops_ref.py と同じ書式)
 *   exfat_dirty.img     VolumeDirty が立っているイメージ (読み取り専用に落ちること)
 * 結果イメージは cache/exfat_rw_result.img に残す。ホストが pull して fsck.exfat と exfat-fuse で独立に検証する。
 */
class ExfatWriteTest {
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
            "write" -> writePattern(vol, f[1], f[2].toLong(), f[3].toLong(), f[4].toInt())
            else -> throw AssertionError("bad op: $line")
        }
    }

    @Test
    fun scriptedOperationsLeaveCleanVolume() {
        val img = File(ctx.cacheDir, "exfat_wr.img")
        val ops = File(ctx.cacheDir, "exfat_wr_ops.txt")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/exfat_wr_ops.txt", ops) && shellCopy("/data/local/tmp/exfat_wr.img", img))
        val result = File(ctx.cacheDir, "exfat_rw_result.img")
        result.delete()
        val vol = mountRw(img)
        var n = 0
        try {
            assertEquals("exfat", vol.fsName)
            assertTrue("exFAT must mount writable", vol.writable)
            for (line in ops.readLines()) {
                try { runOp(vol, line) } catch (ex: Throwable) { throw AssertionError("op #$n failed: $line", ex) }
                n++
            }
            vol.sync()
            // 書いた内容を同じセッション内でも読み戻す (パターン)
            val e = vol.stat("/w/big.bin")
            assertEquals(3_004_999L, e.size)
            val buf = ByteArray(4096)
            assertEquals(4096, vol.read("/w/big.bin", 200_000, buf))
            for (i in buf.indices) assertEquals("big.bin @${200_000 + i}", (((200_000 + i).toLong() * 7 + 6) and 255).toInt(), buf[i].toInt() and 255)
        } finally { vol.close() }
        assertTrue(n > 100)
        img.copyTo(result, overwrite = true)   // 閉じたあとのイメージ (クリーンアンマウント済み: VolumeDirty なし)
        img.delete()
        android.util.Log.i("usbmem", "exfat write test: $n ops done")
    }

    @Test
    fun dirtyVolumeFallsBackToReadOnly() {
        val img = File(ctx.cacheDir, "exfat_dirty.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/exfat_dirty.img", img))
        val vol = mountRw(img)
        try {
            assertEquals("exfat", vol.fsName)
            assertFalse("dirty exFAT must not be writable", vol.writable)
            try { vol.create("/", "x", false); throw AssertionError("create must fail on a read-only volume") } catch (ex: NativeException) { assertEquals(30, ex.code) }
            vol.list("/")   // 読み取りはできる
        } finally { vol.close() }
        // イメージは 1 バイトも変わっていない (VolumeDirty のまま)
        val after = img.readBytes()
        assertEquals(2, (after[106].toInt() and 255) or ((after[107].toInt() and 255) shl 8))
        img.delete()
    }
}
