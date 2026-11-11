package org.usbmem.app

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** JNI 経由で実際に RAW イメージを開く(SAF で得る fd と同じ経路: detachFd → ネイティブ)。 */
class RawImageInstrumentedTest {
    private fun mbrImage(): ByteArray {
        val img = ByteArray(8192 * 512)
        val bb = ByteBuffer.wrap(img).order(ByteOrder.LITTLE_ENDIAN)
        fun entry(slot: Int, type: Int, start: Int, count: Int) {
            val o = 446 + 16 * slot
            img[o + 4] = type.toByte(); bb.putInt(o + 8, start); bb.putInt(o + 12, count)
        }
        entry(0, 0x07, 2048, 4000); entry(1, 0x83, 6048, 2000)
        img[510] = 0x55; img[511] = 0xAA.toByte()
        return img
    }

    private fun open(file: File, mode: Int = ParcelFileDescriptor.MODE_READ_ONLY): RawImage {
        val pfd = ParcelFileDescriptor.open(file, mode)
        return RawImage.openReadOnly(pfd.detachFd())
    }

    @Test
    fun listsMbrPartitions() {
        val f = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "t.img")
        f.writeBytes(mbrImage())
        open(f).use { img ->
            val t = img.partitionTable()
            assertEquals("mbr", t.kind)
            assertEquals(2, t.partitions.size)
            assertEquals(2048L, t.partitions[0].startLba)
            assertEquals(2000L * 512, t.partitions[1].sizeBytes)
            assertEquals("Linux", t.partitions[1].typeName)
        }
    }

    @Test
    fun missingOrBadFdReportsError() {
        try {
            RawImage.openReadOnly(-1)
            fail("expected NativeException")
        } catch (e: NativeException) {
            assertTrue(e.message!!.contains("invalid-argument"))
        }
    }

    @Test
    fun blankImageHasNoPartitionsButDiagnostics() {
        val f = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "blank.img")
        f.writeBytes(ByteArray(64 * 512))
        open(f).use { assertTrue(it.partitionTable().diagnostics.any { d -> d.code == "blank-device" }) }
    }
}
