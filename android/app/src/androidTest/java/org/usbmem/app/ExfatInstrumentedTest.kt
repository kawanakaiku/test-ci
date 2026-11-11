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
 * 実 exFAT イメージ (tools/gen_exfat_fixture.py) を Android 上で fs/exfat 経由で読む (読み取り専用)。
 * 事前に adb push で /data/local/tmp/exfat_<name>.{img,manifest} を置く。manifest はタブ区切り: F path size valid seed / D path。
 * ファイルの内容は byte(i) = (i*31 + seed*7 + (i>>8)) & 0xFF (i < valid)、valid 以降は 0。
 */
class ExfatInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun verify(name: String) {
        val img = File(ctx.cacheDir, "exfat_$name.img")
        val man = File(ctx.cacheDir, "exfat_$name.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/exfat_$name.manifest", man) && shellCopy("/data/local/tmp/exfat_$name.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                assertEquals("exfat", vol.fsName)
                assertFalse(vol.writable)
                var files = 0; var dirs = 0
                for (line in man.readLines()) {
                    val p = line.split("\t")
                    if (p[0] == "D") {
                        assertTrue("dir ${p[1]}", vol.list("/" + p[1]).size >= 0); dirs++
                    } else if (p[0] == "F") {
                        val size = p[2].toLong(); val valid = p[3].toLong(); val seed = p[4].toInt()
                        val buf = ByteArray(65536)
                        var off = 0L
                        while (true) {
                            val n = vol.read(p[1], off, buf)
                            if (n == 0) break
                            for (k in 0 until n) {
                                val i = off + k
                                val want = if (i < valid) ((i * 31 + seed * 7 + (i shr 8)) and 0xFF).toInt() else 0
                                if ((buf[k].toInt() and 0xFF) != want) throw AssertionError("${p[1]} @${i}: ${buf[k].toInt() and 0xFF} != $want")
                            }
                            off += n
                        }
                        assertEquals("size ${p[1]}", size, off)
                        files++
                    }
                }
                assertTrue("files=$files", files >= 300)
                assertTrue(vol.list("/").any { it.name == "日本語ファイル名.txt" })
                android.util.Log.i("usbmem", "exfat $name: $files files, $dirs dirs verified")
            }
        } finally { img.delete(); man.delete() }
    }

    @Test fun mountsAndVerifiesC4k() = verify("c4k")
    @Test fun mountsAndVerifiesC512() = verify("c512")
    @Test fun mountsAndVerifiesLargeClusters() = verify("s4k_c32k")
}
