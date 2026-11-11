package org.usbmem.app

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build

/** USB のインタフェース記述子の最小モデル (Android の final クラスに依存せず JVM でテストできるようにする)。 */
data class UsbEndpointInfo(val address: Int, val isBulk: Boolean) {
    val isIn: Boolean get() = (address and 0x80) != 0
}

data class UsbInterfaceInfo(val number: Int, val cls: Int, val subclass: Int, val protocol: Int, val endpoints: List<UsbEndpointInfo>)

data class MscPick(val interfaceNumber: Int, val epIn: Int, val epOut: Int)

object UsbMscSelector {
    /** Mass Storage (class 0x08) / SCSI transparent (subclass 0x06) / Bulk-Only (protocol 0x50) で、bulk IN/OUT が揃う最初のインタフェース。 */
    fun pick(interfaces: List<UsbInterfaceInfo>): MscPick? {
        for (i in interfaces) {
            if (i.cls != 0x08 || i.subclass != 0x06 || i.protocol != 0x50) continue
            val inEp = i.endpoints.firstOrNull { it.isBulk && it.isIn } ?: continue
            val outEp = i.endpoints.firstOrNull { it.isBulk && !it.isIn } ?: continue
            return MscPick(i.number, inEp.address, outEp.address)
        }
        return null
    }

    fun describe(device: UsbDevice): List<UsbInterfaceInfo> = (0 until device.interfaceCount).map { n ->
        val i = device.getInterface(n)
        UsbInterfaceInfo(
            i.id, i.interfaceClass, i.interfaceSubclass, i.interfaceProtocol,
            (0 until i.endpointCount).map { e ->
                val ep = i.getEndpoint(e)
                UsbEndpointInfo(ep.address, ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK)
            },
        )
    }
}

/**
 * 接続済みの USB マスストレージ。常に読み取り専用で開く (書き込みは検証済みになるまで有効化しない)。
 * close() は NTFS ボリュームを先に閉じてから、ネイティブデバイス → インタフェース解放 → 接続 の順で後始末する。
 */
class UsbStorageSession private constructor(
    val deviceName: String,
    private val conn: UsbDeviceConnection,
    private val intf: UsbInterface,
    private var handle: Long,
    /** true = 書き込み可能で開いている (MSC が WRITE を送れる)。既定は読み取り専用。 */
    val writable: Boolean = false,
) : AutoCloseable {
    private val volumes = mutableListOf<NtfsVolume>()

    fun partitionTable(): PartitionTable {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(handle, err)
            ?: throw NativeException(err[0], "partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        return PartitionTable.parse(String(bytes, Charsets.UTF_8))
    }

    /** 診断用: これまでに送った SCSI オペコードの集計 (例: "12x1 1ax1 25x1 28x4237")。WRITE 系 (2a/8a) や 35 が出ないことの確認用。 */
    fun opStats(): String = if (Handle.valid(handle)) NativeBridge.nativeUsbOpStats(handle) else ""

    /** 診断用: 直近の SCSI センス [key, asc, ascq]。 */
    fun lastSense(): IntArray = IntArray(3).also { if (Handle.valid(handle)) NativeBridge.nativeUsbLastSense(handle, it) }

    @Synchronized
    fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean = false): NtfsVolume {
        check(Handle.valid(handle)) { "closed" }
        if (writable && !this.writable) throw NativeException(30, "このセッションは読み取り専用で開かれています")
        val v = if (writable) NtfsVolume.mountDeviceRangeReadWrite(handle, offsetBytes, sizeBytes) else NtfsVolume.mountDeviceRange(handle, offsetBytes, sizeBytes)
        return v.also { volumes += it }
    }

    /** 転送サイズの調整 (速度計測用)。KiB。0 = 変更しない。 */
    @Synchronized
    fun tune(chunkKiB: Int, maxTransferKiB: Int) {
        check(Handle.valid(handle)) { "closed" }
        val r = NativeBridge.nativeUsbTune(handle, chunkKiB, maxTransferKiB)
        if (r != 0) throw NativeException(-r, "tune failed")
    }

    /** 読み取り専用の速度計測: offset から totalBytes を bufKiB ごとに読み、経過マイクロ秒を返す。 */
    @Synchronized
    fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int): Long {
        check(Handle.valid(handle)) { "closed" }
        val us = NativeBridge.nativeBenchRead(handle, offset, totalBytes, bufKiB)
        if (us <= 0) throw NativeException((-us).toInt(), "bench failed: ${NativeBridge.nativeErrcName((-us).toInt())}")
        return us
    }

    @Synchronized
    override fun close() {
        volumes.forEach { runCatching { it.close() } }
        volumes.clear()
        if (Handle.valid(handle)) NativeBridge.nativeClose(handle)
        handle = 0
        runCatching { conn.releaseInterface(intf) }
        conn.close()
    }

    companion object {
        const val ACTION_PERMISSION = "org.usbmem.app.USB_PERMISSION"

        fun permissionIntent(ctx: Context): PendingIntent {
            // Android 14+ は暗黙のブロードキャスト PendingIntent を拒否するので、必ず自パッケージを明示する。
            val i = Intent(ACTION_PERMISSION).setPackage(ctx.packageName)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            return PendingIntent.getBroadcast(ctx, 0, i, flags)
        }

        /**
         * OS (vold) が現在マウント中の USB マスストレージ由来ボリュームの一覧 (/proc/mounts の "/dev/block/vold/public:8,<minor>" = SCSI ディスク)。
         * claimInterface(force=true) は OS のドライバを強制的に切り離すため、マウント中のボリュームがあると下から引き抜くことになる。
         * 内蔵 SD スロット (mmc = major 179) は対象外。
         */
        fun osMountedUsbVolumes(mountsText: String): List<String> =
            mountsText.lineSequence().map { it.substringBefore(' ') }.filter { it.startsWith("/dev/block/vold/public:8,") }.distinct().toList()

        /** USB パーミッション取得済みのデバイスを読み取り専用で開く。 */
        fun open(ctx: Context, usb: UsbManager, device: UsbDevice, writable: Boolean = false): UsbStorageSession {
            val pick = UsbMscSelector.pick(UsbMscSelector.describe(device)) ?: throw NativeException(9, "Mass Storage (Bulk-Only) インタフェースがありません")
            if (!usb.hasPermission(device)) throw SecurityException("USB permission not granted")
            // 安全策: OS がこのような USB ボリュームをマウント中は、ドライバの強制切り離しをしない (取り出してから再試行する)。
            // /proc/mounts を読めなかった場合は判断できないので、安全側 (拒否) に倒す。
            val mounts = try { java.io.File("/proc/mounts").readText() } catch (_: Exception) { throw NativeException(16, "/proc/mounts を読めないため、OS のマウント状態を確認できません") }
            val busy = osMountedUsbVolumes(mounts)
            if (busy.isNotEmpty()) throw NativeException(16, "OS が USB ボリュームをマウント中です(${busy.size} 個)。先に「取り出し」てから再試行してください")
            val intf = (0 until device.interfaceCount).map { device.getInterface(it) }.first { it.id == pick.interfaceNumber }
            val conn = usb.openDevice(device) ?: throw NativeException(2, "USB デバイスを開けません")
            // 他ドライバ(カーネルの usb-storage など)が掴んでいれば force=true で奪う。Android では通常は掴まれていない。
            if (!conn.claimInterface(intf, true)) { conn.close(); throw NativeException(2, "インタフェースを確保できません") }
            val h = NativeBridge.nativeOpenUsbMsc(conn.fileDescriptor, pick.epIn, pick.epOut, pick.interfaceNumber, 0, !writable)
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                // 失敗の原因調査用 (SCSI コマンドと結果のみ。個人情報なし)
                android.util.Log.w("usbmem", "USB open failed: ${NativeBridge.nativeErrcName(code)} trace=${NativeBridge.nativeUsbLastOpenTrace()}")
                runCatching { conn.releaseInterface(intf) }
                conn.close()
                throw NativeException(code, "USB 初期化に失敗: ${NativeBridge.nativeErrcName(code)}")
            }
            return UsbStorageSession(device.deviceName, conn, intf, h, writable)
        }
    }
}
