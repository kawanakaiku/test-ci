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
 * FAT12/16/32 (vfat) の書き込み (実験的。-PusbmemNtfsWrite=true のビルドだけ) を Android 上で fs/fat 経由で行う。tools/run_android_fat_write_test.sh から実行する。docs/fat-write.md
 * ホストが /data/local/tmp に置くもの:
 *   fat_wr_list.txt             "名前 大きいファイルの基準サイズ" の行 (例: fat16_c512 3000000)
 *   fat_wr_<名前>.img           書き込み対象のフィクスチャ (tools/gen_fat_fixture.py)
 *   fat_wr_<名前>_ops.txt       操作列 (tools/gen_fat_ops.py。usbmem_fat_write_tests --ops / tools/ops_ref.py と同じ書式)
 *   fat_dirty_boot.img          ブートセクタのダーティビットが立っているイメージ (読み取り専用に落ちること)
 *   fat_dirty_fat.img           FAT[1] のクリーン bit が落ちているイメージ (読み取り専用に落ちること)
 * 結果イメージは cache/fat_rw_<名前>.img に残す。ホストが pull して fsck.fat・7-Zip・mtools で独立に検証する。
 */
class FatWriteTest {
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
        val list = File(ctx.cacheDir, "fat_wr_list.txt")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/fat_wr_list.txt", list))
        for (entry in list.readLines().filter { it.isNotBlank() }) {
            val (name, bigStr) = entry.trim().split(Regex("\\s+"))
            val big = bigStr.toLong()
            val img = File(ctx.cacheDir, "fat_wr_$name.img")
            val ops = File(ctx.cacheDir, "fat_wr_${name}_ops.txt")
            assertTrue("fixture $name not pushed", shellCopy("/data/local/tmp/fat_wr_${name}_ops.txt", ops) && shellCopy("/data/local/tmp/fat_wr_$name.img", img))
            val result = File(ctx.cacheDir, "fat_rw_$name.img")
            result.delete()
            val vol = mountRw(img)
            var n = 0
            try {
                assertEquals("fat", vol.fsName)
                assertTrue("FAT must mount writable ($name)", vol.writable)
                for (line in ops.readLines()) {
                    try { runOp(vol, line) } catch (ex: Throwable) { throw AssertionError("[$name] op #$n failed: $line", ex) }
                    n++
                }
                vol.sync()
                // 書いた内容を同じセッション内でも読み戻す (パターン)
                val e = vol.stat("/w/big.bin")
                assertEquals(big + 4999, e.size)
                val buf = ByteArray(4096)
                assertEquals(4096, vol.read("/w/big.bin", 200_000, buf))
                for (i in buf.indices) assertEquals("[$name] big.bin @${200_000 + i}", (((200_000 + i).toLong() * 7 + 6) and 255).toInt(), buf[i].toInt() and 255)
                // 255 文字の名前・日本語名も同じセッションで引ける
                assertEquals(10L, vol.stat("/w/" + "x".repeat(255)).size)
                assertEquals(10L, vol.stat("/w/" + "あ".repeat(85)).size)
            } finally { vol.close() }
            assertTrue(n > 100)
            img.copyTo(result, overwrite = true)   // 閉じたあとのイメージ (クリーンアンマウント済み: ダーティ印なし)
            img.delete()
            android.util.Log.i("usbmem", "fat write test [$name]: $n ops done")
        }
    }

    private fun sha256Of(f: File): String {
        val md = java.security.MessageDigest.getInstance("SHA-256")
        FileInputStream(f).use { ins -> val buf = ByteArray(1 shl 20); while (true) { val n = ins.read(buf); if (n < 0) break; md.update(buf, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    private fun assertReadOnlyFallback(file: String, what: String) {
        val img = File(ctx.cacheDir, file)
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/$file", img))
        val before = sha256Of(img)   // AVD のヒープ上限 (192MB) のため、丸ごと読まずストリームのハッシュで比べる
        val vol = mountRw(img)
        try {
            assertEquals("fat", vol.fsName)
            assertFalse("$what FAT must not be writable", vol.writable)
            try { vol.create("/", "x", false); throw AssertionError("create must fail on a read-only volume") } catch (ex: NativeException) { assertEquals(30, ex.code) }
            vol.list("/")   // 読み取りはできる
        } finally { vol.close() }
        // イメージは 1 バイトも変わっていない (ダーティのまま)
        assertEquals("$what image must stay byte-identical", before, sha256Of(img))
        img.delete()
    }

    @Test
    fun bootSectorDirtyFallsBackToReadOnly() = assertReadOnlyFallback("fat_dirty_boot.img", "boot-dirty")

    @Test
    fun fatCleanBitMissingFallsBackToReadOnly() = assertReadOnlyFallback("fat_dirty_fat.img", "FAT[1]-unclean")
}
