package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class UsbMscSelectorTest {
    private fun ep(addr: Int, bulk: Boolean = true) = UsbEndpointInfo(addr, bulk)

    @Test fun picksBotScsiInterfaceWithBulkEndpoints() {
        val p = UsbMscSelector.pick(listOf(UsbInterfaceInfo(2, 8, 6, 0x50, listOf(ep(0x81), ep(0x02)))))
        assertEquals(MscPick(2, 0x81, 0x02), p)
    }

    @Test fun skipsNonStorageAndUasAndNonBulk() {
        val ifs = listOf(
            UsbInterfaceInfo(0, 3, 1, 1, listOf(ep(0x81, false))),            // HID
            UsbInterfaceInfo(1, 8, 6, 0x62, listOf(ep(0x81), ep(0x02))),      // UAS (未対応)
            UsbInterfaceInfo(2, 8, 6, 0x50, listOf(ep(0x81, false), ep(0x02))), // IN が bulk でない
            UsbInterfaceInfo(3, 8, 4, 0x50, listOf(ep(0x81), ep(0x02))),      // UFI (未対応)
            UsbInterfaceInfo(4, 8, 6, 0x50, listOf(ep(0x83), ep(0x04))),      // これが対象
        )
        assertEquals(MscPick(4, 0x83, 0x04), UsbMscSelector.pick(ifs))
    }

    @Test fun returnsNullWhenOutEndpointMissing() {
        assertNull(UsbMscSelector.pick(listOf(UsbInterfaceInfo(0, 8, 6, 0x50, listOf(ep(0x81))))))
    }
}
