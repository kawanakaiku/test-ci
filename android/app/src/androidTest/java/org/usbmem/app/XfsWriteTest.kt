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
 * XFS の書き込み (実験的。-PusbmemNtfsWrite=true のビルドだけ。CreateOnly の範囲: 新規作成・追記・mkdir・symlink) を Android 上で自前のエンジン (core/compat/src/xfs_write*.inc) 経由で行う。
 * tools/run_android_xfs_write_test.sh から実行する。ホストが /data/local/tmp に置くもの:
 *   xfs_wr.img          書き込み対象のイメージ (mkfs.xfs -p で元ファイル入り。tools/gen_xfs_ops.py seed)
 *   xfs_wr_ops.txt      操作列 (tools/gen_xfs_ops.py ops ... createonly。usbmem_xfs_write_tests --ops / tools/ops_ref.py と同じ書式)
 *   xfs_rsv.img         書き込み拒否のイメージ (rmapbt=1: 読み取り専用に落ちること)
 * 結果イメージは cache/xfs_rw_result.img に残す。ホストが pull して xfs_repair -n で独立に検証し、同じ操作列を POSIX 操作として実行した結果とツリー全体を比べる。
 * XFS は Full を要求しても createOnly で開く (NativeBridge.xfsCreateOnlyOnly) ので、既存ファイルへの上書き・削除・rename はここでは行わない。
 */
class XfsWriteTest {
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

    /** 320 MiB のイメージをまるごと読むとヒープが足りないので、ストリームでハッシュする。 */
    private fun sha256(f: File): List<Byte> {
        val md = java.security.MessageDigest.getInstance("SHA-256")
        f.inputStream().use { ins -> val b = ByteArray(1 shl 20); while (true) { val n = ins.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return md.digest().toList()
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
        val img = File(ctx.cacheDir, "xfs_wr.img")
        val ops = File(ctx.cacheDir, "xfs_wr_ops.txt")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_wr_ops.txt", ops) && shellCopy("/data/local/tmp/xfs_wr.img", img))
        val result = File(ctx.cacheDir, "xfs_rw_result.img")
        result.delete()
        val vol = mountRw(img)
        var n = 0
        try {
            assertEquals("xfs", vol.fsName)
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
            // 既存ファイルの上書き・削除・リンクは (createOnly なので) 拒否
            try { vol.write("/tiny.txt", 0, ByteArray(4)); throw AssertionError("overwrite must be denied") } catch (ex: NativeException) { }
            try { vol.remove("/tiny.txt"); throw AssertionError("remove must be denied") } catch (ex: NativeException) { }
        } finally { vol.close() }
        assertTrue(n > 100)
        img.copyTo(result, overwrite = true)   // 閉じたあとのイメージ (コミット済み)
        img.delete()
        android.util.Log.i("usbmem", "xfs write test: $n ops done")
    }

    /** rmapbt=1 のイメージは書き込み拒否 (読み取り専用で開ける。読み取りはでき、書き込みは EROFS=30、イメージは 1 バイトも変わらない)。 */
    @Test
    fun rmapbtVolumeFallsBackToReadOnly() {
        val img = File(ctx.cacheDir, "xfs_rsv.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_rsv.img", img))
        val before = sha256(img)
        val vol = mountRw(img)
        try {
            assertEquals("xfs", vol.fsName)
            assertFalse("a volume with rmapbt must not be writable", vol.writable)
            try { vol.create("/", "x", false); throw AssertionError("create must fail on a read-only volume") } catch (ex: NativeException) { assertEquals(30, ex.code) }
            vol.list("/")   // 読み取りはできる
        } finally { vol.close() }
        assertEquals(before, sha256(img))
        img.delete()
    }

    /** 書き込み可でマウントしても、書かずに閉じればイメージは 1 バイトも変わらない。 */
    @Test
    fun rwMountWithoutWritesLeavesImageUnchanged() {
        val img = File(ctx.cacheDir, "xfs_noop.img")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/xfs_wr.img", img))
        val before = sha256(img)
        val vol = mountRw(img)
        try {
            assertEquals("xfs", vol.fsName)
            assertTrue(vol.writable)
            vol.list("/")
        } finally { vol.close() }
        assertEquals(before, sha256(img))
        img.delete()
    }
}
