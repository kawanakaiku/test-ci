package org.usbmem.app

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * 実 NTFS イメージ(tools/gen_ntfs_fixture.sh)を Android 上で fs/ntfs 経由で読む。
 * 事前に adb push で /data/local/tmp/ntfs_basic.{img,manifest} を置く。アプリは shell の tmp を直接読めないので
 * UiAutomation(shell 権限)の cat で自分の cacheDir へコピーする。
 */
class NtfsInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    @Test
    fun mountsAndVerifiesManifest() {
        val img = File(ctx.cacheDir, "ntfs_basic.img")
        val man = File(ctx.cacheDir, "ntfs_basic.manifest")
        assumeTrue("fixture not pushed", shellCopy("/data/local/tmp/ntfs_basic.manifest", man) && shellCopy("/data/local/tmp/ntfs_basic.img", img))
        try {
            val pfd = ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY)
            NtfsVolume.mountReadOnly(pfd.detachFd()).use { vol ->
                val root = vol.list("/").map { it.name }.toSet()
                assertTrue(root.containsAll(setOf("a.txt", "big.bin", "dir1", "日本語.txt", "many")))
                assertEquals(800, vol.list("/many").size)
                var files = 0
                for (line in man.readLines()) {
                    val p = line.split(" ", limit = 4)
                    if (p[0] != "f" || p[3].startsWith("many/")) continue
                    val md = MessageDigest.getInstance("SHA-256")
                    val buf = ByteArray(65536)
                    var off = 0L
                    while (true) {
                        val n = vol.read(p[3], off, buf)
                        if (n == 0) break
                        md.update(buf, 0, n); off += n
                    }
                    assertEquals("size ${p[3]}", p[1].toLong(), off)
                    assertEquals("sha ${p[3]}", p[2], md.digest().joinToString("") { "%02x".format(it) })
                    files++
                }
                assertTrue(files >= 5)
            }
        } finally { img.delete(); man.delete() }
    }

    @Test
    fun notNtfsFailsCleanly() {
        val f = File(ctx.cacheDir, "junk.img"); f.writeBytes(ByteArray(1 shl 20))
        try {
            NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY).detachFd()).close()
            throw AssertionError("should not mount")
        } catch (e: NativeException) { assertTrue(e.code != 0) } finally { f.delete() }
    }
}
