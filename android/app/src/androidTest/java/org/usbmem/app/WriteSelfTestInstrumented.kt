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

/** 書き込み自己診断 (WriteSelfTest) の検証: 通ること、既存のファイルに触れないこと、作ったものを全部消すこと、書き込み不可の条件で実行を拒むこと。 */
class WriteSelfTestInstrumented {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun copy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { i -> dst.outputStream().use { i.copyTo(it) } }
        pfd.close(); return dst.length() > 0
    }

    private fun mount(img: File, createOnly: Boolean): VolumeRegistry.Entry = try {
        VolumeRegistry.addFd("StNTFS", "st-key-$createOnly", ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), writable = true, createOnly = createOnly)
    } catch (ex: NativeException) { assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30); throw ex }

    @Test
    fun passesAndLeavesNoTrace() {
        val img = File(ctx.cacheDir, "selftest.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val e = mount(img, createOnly = false)
        assumeTrue("volume mounted read-only", e.volume.writable)
        try {
            val before = e.volume.list("/").map { it.name to it.size }.toSet()
            val r = WriteSelfTest.run(e.volume, "20260101_000000")
            for (s in r.steps) android.util.Log.i("usbmem", "selftest step '${s.name}' ok=${s.ok} ${s.ms}ms ${s.detail}")
            assertTrue("failed: ${r.failedStep}", r.passed)
            assertTrue(r.steps.size >= 8)
            assertEquals(before, e.volume.list("/").map { it.name to it.size }.toSet())   // ルートは元のまま (一時フォルダは消えた)
        } finally { VolumeRegistry.remove(ctx, e.id); img.delete() }
    }

    @Test
    fun refusesWhenCreateOnlyAndNeverTouchesExistingFolder() {
        val img = File(ctx.cacheDir, "selftest2.img")
        assumeTrue("fixture not pushed", copy("/data/local/tmp/ntfs_basic.img", img))
        val co = mount(img, createOnly = true)
        try {
            val r = WriteSelfTest.run(co.volume, "x")
            assertFalse(r.passed)
            assertEquals("precondition", r.steps.first().name)
        } finally { VolumeRegistry.remove(ctx, co.id) }
        // 同名の既存フォルダがあれば、実行を拒み、そのフォルダを消さない
        val full = mount(img, createOnly = false)
        try {
            full.volume.create("/", "usbmem_selftest_dup", true)
            full.volume.create("/usbmem_selftest_dup", "keep.txt", false)
            val r = WriteSelfTest.run(full.volume, "dup")
            assertFalse(r.passed)
            assertTrue(full.volume.list("/usbmem_selftest_dup").any { it.name == "keep.txt" })
        } finally { VolumeRegistry.remove(ctx, full.id); img.delete() }
    }
}
