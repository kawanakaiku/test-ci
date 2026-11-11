package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** OS がマウント中の USB ボリュームを検出する安全策 (実機で EFI パーティションが自動マウントされていた事例から)。 */
class OsMountGuardTest {
    @Test
    fun detectsScsiDiskVolumesOnly() {
        val mounts = """
            /dev/block/sda13 on /metadata type ext4 (rw)
            /dev/block/vold/public:179,1 on /mnt/media_rw/FFD9-7D43 type texfat (rw)
            /dev/block/vold/public:8,130 on /mnt/media_rw/0922-588F type vfat (rw,dirsync)
            /mnt/media_rw/0922-588F on /mnt/runtime/default/0922-588F type sdcardfs (rw)
            /dev/block/vold/public:8,130 on /somewhere/else type vfat (rw)
        """.trimIndent().replace(" on ", " on ")
        // /proc/mounts の実形式は "<dev> <mountpoint> <type> ..." (空白区切り)。先頭フィールドだけを見る
        val real = mounts.lines().joinToString("\n") { it.replace(" on ", " ") }
        assertEquals(listOf("/dev/block/vold/public:8,130"), UsbStorageSession.osMountedUsbVolumes(real))
    }

    @Test
    fun emptyWhenNoUsbVolumes() {
        assertTrue(UsbStorageSession.osMountedUsbVolumes("/dev/block/vold/public:179,1 /mnt/media_rw/X texfat rw 0 0\ntmpfs /storage tmpfs rw 0 0\n").isEmpty())
        assertTrue(UsbStorageSession.osMountedUsbVolumes("").isEmpty())
    }
}
