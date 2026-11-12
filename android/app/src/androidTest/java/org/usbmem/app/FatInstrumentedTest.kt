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
import java.util.TimeZone

/**
 * 実 FAT12/16/32 イメージ (tools/gen_fat_fixture.py。mtools で作り、7z と mtools の読み出しで検証済み) を Android 上で fs/fat 経由で読む (読み取り専用)。
 * 事前に adb push で /data/local/tmp/fat_<name>.{img,manifest} を置く。manifest はタブ区切り:
 *   D path / F path size seed mtime attrs  (mtime = FAT のフィールドを UTC とみなした秒、attrs = r 読み取り専用 h 隠し s システム または -)
 * ファイルの内容は byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF。
 * FAT の時刻は書いた端末のローカル時刻なので、JNI は「現在の端末のタイムゾーン」で UTC に直す。期待値は manifest の mtime からその差し引きをした値。
 */
class FatInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun verify(name: String, minFiles: Int) {
        val img = File(ctx.cacheDir, "fat_$name.img")
        val man = File(ctx.cacheDir, "fat_$name.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/fat_$name.manifest", man) && shellCopy("/data/local/tmp/fat_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("fat", vol.fsName)
                assertFalse(vol.writable)
                // 端末の現在のタイムゾーンでの UTC との差 (秒)。JNI は localtime_r の tm_gmtoff を使う (全ての日付に同じ値)
                val offsetNow = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000L
                var files = 0; var dirs = 0; var readonly = 0
                for (line in man.readLines()) {
                    if (line.startsWith("#")) continue
                    val p = line.split("\t")
                    if (p[0] == "D") {
                        val st = vol.stat("/" + p[1])
                        assertTrue("dir ${p[1]}", st.isDir)
                        vol.list("/" + p[1]); dirs++
                    } else if (p[0] == "F") {
                        val size = p[2].toLong(); val seed = p[3].toInt(); val mtime = p[4].toLong(); val attrs = p[5]
                        val st = vol.stat("/" + p[1])
                        assertFalse("file ${p[1]}", st.isDir)
                        assertEquals("size ${p[1]}", size, st.size)
                        assertEquals("mtime ${p[1]}", mtime - offsetNow, st.mtimeSec)
                        if ('r' in attrs) readonly++
                        val buf = ByteArray(65536)
                        var off = 0L
                        while (true) {
                            val n = vol.read("/" + p[1], off, buf)
                            if (n == 0) break
                            for (k in 0 until n) {
                                val i = off + k
                                val want = ((i * 31 + seed * 7 + (i shr 8)) and 0xFF).toInt()
                                if ((buf[k].toInt() and 0xFF) != want) throw AssertionError("${p[1]} @${i}: ${buf[k].toInt() and 0xFF} != $want")
                            }
                            off += n
                        }
                        assertEquals("read size ${p[1]}", size, off)
                        files++
                    }
                }
                assertTrue("files=$files", files >= minFiles)
                assertTrue("readonly=$readonly", readonly >= 2)
                val root = vol.list("/").map { it.name }
                assertTrue(root.contains("日本語ファイル名.txt"))   // 長い名前 (UTF-16)
                assertTrue(root.contains("emoji_😀.txt"))  // サロゲートペア
                assertTrue(root.contains("lower.txt") && root.contains("SMALL.TXT") && root.contains("MiXeD.CaSe"))   // 短い名前の大文字小文字
                assertTrue(vol.list("/").any { it.name == "日本語ディレクトリ" && it.isDir })
                android.util.Log.i("usbmem", "fat $name: $files files, $dirs dirs verified")
            }
        } finally { img.delete(); man.delete() }
    }

    @Test fun fat12Floppy() = verify("fat12_f1440", 140)
    @Test fun fat12Clusters4k() = verify("fat12_c4k", 300)
    @Test fun fat16Clusters512() = verify("fat16_c512", 300)
    @Test fun fat16Clusters2k() = verify("fat16_c2k", 300)
    @Test fun fat32Clusters512() = verify("fat32_c512", 300)
    @Test fun fat32Clusters4k() = verify("fat32_c4k", 300)
}
