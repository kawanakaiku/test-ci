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
 * ext4 の書き込み (fs/ext4 + fs/jbd2。実験的。-PusbmemNtfsWrite=true のビルドだけ) を Android 上で行う。tools/run_android_ext4_write_test.sh から実行する。docs/ext4-write.md
 * ホストが /data/local/tmp に置くもの:
 *   ext4_wr.img        書き込み対象のフィクスチャ (tools/gen_ext4_fixture.py の e4_def1k: metadata_csum / 64bit / flex_bg / orphan_file / ジャーナル)
 *   ext4_wr_ops.txt    操作列 (tools/gen_ext2_ops.py。usbmem_ext4_write_tests --ops / tools/ops_ref.py と同じ書式)
 *   ext4_dirty.img     s_state が VALID_FS でないイメージ (読み取り専用に落ちること)
 *   ext4_journal.img   debugfs で未再生のトランザクションを書いたイメージ (needs_recovery。書き込み可のマウントで再生されること)
 * 結果イメージは cache/ext4_rw_result.img / cache/ext4_replay_result.img に残す。ホストが pull して e2fsck と fuse2fs で独立に検証する。
 */
class Ext4WriteTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun mountRw(img: File, createOnly: Boolean = false): NtfsVolume {
        val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE)
        try {
            return NtfsVolume.mountReadWrite(pfd.detachFd(), createOnly = createOnly)
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
            "write" -> {
                val off = f[2].toLong()
                // ops_ref.py は EOF より先への書き込みの隙間をゼロで埋める (疎)。こちらは書き込みが EOF を超える位置から始まれば自動で穴になる
                writePattern(vol, f[1], off, f[3].toLong(), f[4].toInt())
            }
            else -> throw AssertionError("bad op: $line")
        }
    }

    @Test
    fun scriptedOperationsLeaveCleanVolume() {
        val img = File(ctx.cacheDir, "ext4_wr.img")
        val ops = File(ctx.cacheDir, "ext4_wr_ops.txt")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_wr_ops.txt", ops) && shellCopy("/data/local/tmp/ext4_wr.img", img))
        val result = File(ctx.cacheDir, "ext4_rw_result.img")
        result.delete()
        val vol = mountRw(img)
        var n = 0
        try {
            assertEquals("ext4", vol.fsName)
            assertTrue("ext4 must mount writable", vol.writable)
            for (line in ops.readLines()) {
                try { runOp(vol, line) } catch (ex: Throwable) { throw AssertionError("op #$n failed: $line", ex) }
                n++
            }
            vol.sync()
            val e = vol.stat("/w/big.bin")
            assertEquals(5_304_999L, e.size)
            val buf = ByteArray(4096)
            assertEquals(4096, vol.read("/w/big.bin", 200_000, buf))
            for (i in buf.indices) assertEquals("big.bin @${200_000 + i}", (((200_000 + i).toLong() * 7 + 6) and 255).toInt(), buf[i].toInt() and 255)
            assertEquals("/etc/passwd", vol.readlink("/w/fast.lnk"))
        } finally { vol.close() }
        assertTrue(n > 100)
        img.copyTo(result, overwrite = true)   // 閉じたあとのイメージ (クリーンアンマウント済み: needs_recovery が消えている)
        img.delete()
        android.util.Log.i("usbmem", "ext4 write test: $n ops done")
    }

    @Test
    fun createOnlyModeRefusesChangesToExistingData() {
        val img = File(ctx.cacheDir, "ext4_co.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_wr.img", img))
        val vol = mountRw(img, createOnly = true)
        try {
            assertTrue(vol.writable)
            vol.create("/", "co_dir", true)
            vol.create("/co_dir", "new.bin", false)
            writePattern(vol, "/co_dir/new.bin", 0, 100_000, 3)
            vol.symlink("/co_dir", "l", "/small.txt")
            for (bad in listOf<() -> Unit>({ vol.remove("/small.txt") }, { vol.truncate("/small.txt", 1) }, { vol.write("/small.txt", 0, ByteArray(1)) },
                                          { vol.move("/small.txt", "/", "moved.txt") }, { vol.link("/co_dir", "h", "/small.txt") })) {
                try { bad(); throw AssertionError("must be refused in create-only mode") } catch (ex: NativeException) { /* 拒否 */ }
            }
            vol.sync()
            assertEquals(100_000L, vol.stat("/co_dir/new.bin").size)
        } finally { vol.close() }
        img.delete()
    }

    @Test
    fun uncleanVolumeFallsBackToReadOnly() {
        val img = File(ctx.cacheDir, "ext4_dirty.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_dirty.img", img))
        val before = img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() }
        val vol = mountRw(img)
        try {
            assertEquals("ext4", vol.fsName)
            assertFalse("unclean ext4 must not be writable", vol.writable)
            try { vol.create("/", "x", false); throw AssertionError("create must fail on a read-only volume") } catch (ex: NativeException) { assertEquals(30, ex.code) }
            vol.list("/")
        } finally { vol.close() }
        assertEquals(before, img.readBytes().let { java.security.MessageDigest.getInstance("SHA-256").digest(it).toList() })   // 1 バイトも変わらない
        img.delete()
    }

    @Test
    fun pendingJournalIsReplayedOnWritableMount() {
        val img = File(ctx.cacheDir, "ext4_journal.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ext4_journal.img", img))
        val result = File(ctx.cacheDir, "ext4_replay_result.img")
        result.delete()
        val vol = mountRw(img)
        try {
            assertEquals("ext4", vol.fsName)
            assertTrue(vol.writable)
            vol.list("/")
        } finally { vol.close() }
        img.copyTo(result, overwrite = true)
        img.delete()
    }
}
