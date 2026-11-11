package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PartitionModelTest {
    @Test
    fun parsesNativeJsonIncludingUnsigned64bit() {
        val json = """{"kind":"gpt","sectorSize":512,"diskBytes":"18446744073709551615","diskGuid":"G",
          "usedBackupGpt":false,"wholeDeviceVolume":false,
          "partitions":[{"index":1,"startLba":"2048","sectorCount":"4000","startBytes":"1048576","sizeBytes":"2048000",
          "logical":false,"extendedContainer":false,"protective":false,"bootable":false,"mbrType":0,
          "typeGuid":"T","uniqueGuid":"U","name":"データ😀","typeName":"Microsoft basic data","fs":"ntfs"}],
          "diagnostics":[{"severity":"warning","code":"c","message":"m"}]}"""
        val t = PartitionTable.parse(json)
        assertEquals(-1L, t.diskBytes) // ULong 最大値は Long では -1 (符号なしとして扱う側の責務)
        assertEquals("データ😀", t.partitions[0].name)
        assertEquals(2048000L, t.partitions[0].sizeBytes)
        assertEquals("ntfs", t.partitions[0].fs)
        assertTrue(t.diagnostics.single().severity == "warning")
    }

    @Test
    fun formatsBytes() {
        assertEquals("512 B", formatBytes(512))
        assertEquals("1.0 MiB", formatBytes(1 shl 20))
    }
}
