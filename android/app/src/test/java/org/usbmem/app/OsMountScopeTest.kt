package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File
import java.nio.file.Files

/** 複数の USB を使うとき、別の USB (まだ開いていないメモリ) の OS マウントを理由に、このデバイスを拒否しない。持ち主が分からないものは従来どおり妨げる。 */
class OsMountScopeTest {
    private val mounts = """
        /dev/block/vold/public:179,1 /mnt/media_rw/SD texfat rw 0 0
        /dev/block/vold/public:8,1 /mnt/media_rw/AAAA vfat rw 0 0
        /dev/block/vold/public:8,17 /mnt/media_rw/BBBB vfat rw 0 0
        /dev/block/vold/public:8,33 /mnt/media_rw/CCCC vfat rw 0 0
    """.trimIndent()
    private val devA = "/dev/bus/usb/001/002"
    private val devB = "/dev/bus/usb/001/003"

    @Test fun onlyTheVolumesOfThisDeviceBlockIt() {
        val owner = mapOf("/dev/block/vold/public:8,1" to devA, "/dev/block/vold/public:8,17" to devB, "/dev/block/vold/public:8,33" to devB)
        assertEquals(listOf("/dev/block/vold/public:8,1"), UsbStorageSession.osMountedBlocking(mounts, devA) { owner[it] })
        assertEquals(listOf("/dev/block/vold/public:8,17", "/dev/block/vold/public:8,33"), UsbStorageSession.osMountedBlocking(mounts, devB) { owner[it] })
        assertEquals(emptyList<String>(), UsbStorageSession.osMountedBlocking(mounts, "/dev/bus/usb/001/009") { owner[it] })
    }

    @Test fun unknownOwnerBlocksEveryDevice() {
        // 持ち主を調べられない (sysfs が読めない) ボリュームは、安全側に倒して、どのデバイスの open も妨げる (従来の挙動)
        val owner = mapOf("/dev/block/vold/public:8,1" to devA)
        assertEquals(listOf("/dev/block/vold/public:8,17", "/dev/block/vold/public:8,33"), UsbStorageSession.osMountedBlocking(mounts, "/dev/bus/usb/001/009") { owner[it] })
        assertEquals(3, UsbStorageSession.osMountedBlocking(mounts, devA) { null }.size)
    }

    @Test fun sysfsOwnerIsTheNearestUsbDeviceDirectory() {
        val root = Files.createTempDirectory("sysfs").toFile()
        try {
            // /sys/devices/.../usb1 (ルートハブ) / 1-1 (ハブ) / 1-1.2 (メモリ) / 1-1.2:1.0 (インタフェース) / host0/.../block/sda/sda1
            val hub = File(root, "usb1/1-1").apply { mkdirs() }
            File(root, "usb1").let { File(it, "busnum").writeText("1\n"); File(it, "devnum").writeText("1\n") }
            File(hub, "busnum").writeText("1\n"); File(hub, "devnum").writeText("2\n")
            val stick = File(hub, "1-1.2").apply { mkdirs() }
            File(stick, "busnum").writeText("1\n"); File(stick, "devnum").writeText("7\n")
            val part = File(stick, "1-1.2:1.0/host0/target0:0:0/0:0:0:0/block/sda/sda1").apply { mkdirs() }
            assertEquals("/dev/bus/usb/001/007", UsbStorageSession.usbOwnerOfSysfsPath(part))
            // USB でない (busnum/devnum が見つからない) 場合は分からない
            val plain = File(root, "platform/mmc/block/mmcblk0p1").apply { mkdirs() }
            assertNull(UsbStorageSession.usbOwnerOfSysfsPath(plain))
        } finally { root.deleteRecursively() }
    }
}
