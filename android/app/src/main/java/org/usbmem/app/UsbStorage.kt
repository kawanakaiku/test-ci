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
 * 1 本の USB 接続 (UsbDeviceConnection + 確保済みのインタフェース + ネイティブの bus) を、同じデバイスの複数 LUN のセッションで共有する。
 * claimInterface はデバイス (インタフェース) 単位なので、LUN ごとに確保し直さず、1 つの接続を参照カウントで共有する:
 * 参照 = LUN ごとのセッション + LUN 一覧のために保つ「調査の保持」。0 になったら bus → インタフェース解放 → 接続 の順に後始末する
 * (片方の LUN を閉じてももう片方は使える)。GET MAX LUN は、接続を開いた最初の 1 回だけ送る。docs/multi-usb.md
 */
class UsbLink internal constructor(
    val deviceName: String,
    private val usb: UsbManager,
    private val queryMaxLun: Boolean,
    private val onShutdown: (UsbLink) -> Unit,
) {
    private var conn: UsbDeviceConnection? = null
    private var intf: UsbInterface? = null
    var bus: Long = 0L
        private set
    /** GET MAX LUN で分かった LUN 数 (応答しない・STALL・不正な値の機器は 1)。開いた後だけ有効 */
    var lunCount: Int = 1
        private set
    private var opened = false
    private var shut = false
    private var refs = 0
    private var sessions = 0
    private var scanHeld = false

    /** 参照を 1 つ取る。すでに後始末済み (参照が 0 になった) なら false: 呼び出し側は新しい接続を作る。 */
    @Synchronized internal fun tryRef(): Boolean { if (shut) return false; refs++; return true }

    /** 取った参照を手放す (0 になれば後始末)。 */
    @Synchronized internal fun unref() { refs--; if (refs <= 0) shutdown() }

    /** 取った参照を「セッション 1 つ」に変える (セッションが閉じるとき [sessionClosed] で手放す)。 */
    @Synchronized internal fun markSession() { sessions++ }

    /** セッションが閉じた。最後のセッションなら、LUN 一覧のための保持も手放す (= OS に返す: 閉じると OS の usb-storage が戻る従来の動作)。 */
    @Synchronized internal fun sessionClosed() {
        sessions--; refs--
        if (sessions <= 0 && scanHeld) { scanHeld = false; refs-- }
        if (refs <= 0) shutdown()
    }

    /** 取った参照を「LUN 一覧のための保持」に変える (すでに保持していれば、余分な参照を手放すだけ)。 */
    @Synchronized internal fun keepForScan() { if (scanHeld) { refs-- } else scanHeld = true }

    /** LUN 一覧のための保持を手放す。セッションが開いていれば、それが閉じるまで接続は保たれる。 */
    @Synchronized internal fun releaseHold() { if (scanHeld) { scanHeld = false; refs--; if (refs <= 0) shutdown() } }

    /** 物理的に開く (1 回だけ): OS マウントの検査 → 接続 → インタフェースの確保 → ネイティブの bus (GET MAX LUN を 1 回)。 */
    @Synchronized internal fun ensureOpen(device: UsbDevice) {
        if (opened) return
        val pick = UsbMscSelector.pick(UsbMscSelector.describe(device)) ?: throw NativeException(9, "Mass Storage (Bulk-Only) インタフェースがありません")
        if (!usb.hasPermission(device)) throw SecurityException("USB permission not granted")
        // 安全策: OS がこのような USB ボリュームをマウント中は、ドライバの強制切り離しをしない (取り出してから再試行する)。
        // 検査はデバイス単位: 同じデバイスのどの LUN でも OS がマウントしていれば、強制 claim で下から引き抜くことになる。
        // /proc/mounts を読めなかった場合は判断できないので、安全側 (拒否) に倒す。
        val mounts = try { java.io.File("/proc/mounts").readText() } catch (_: Exception) { throw NativeException(16, "/proc/mounts を読めないため、OS のマウント状態を確認できません") }
        val busy = UsbStorageSession.osMountedBlocking(mounts, device.deviceName) { UsbStorageSession.sysfsOwnerOfVoldNode(it) }
        if (busy.isNotEmpty()) throw NativeException(16, "OS が USB ボリュームをマウント中です(${busy.size} 個)。先に「取り出し」てから再試行してください")
        val intf = (0 until device.interfaceCount).map { device.getInterface(it) }.first { it.id == pick.interfaceNumber }
        val conn = usb.openDevice(device) ?: throw NativeException(2, "USB デバイスを開けません")
        // 他ドライバ(カーネルの usb-storage など)が掴んでいれば force=true で奪う。Android では通常は掴まれていない。
        if (!conn.claimInterface(intf, true)) { conn.close(); throw NativeException(2, "インタフェースを確保できません") }
        val b = NativeBridge.nativeUsbBusOpen(conn.fileDescriptor, pick.epIn, pick.epOut, pick.interfaceNumber, queryMaxLun)
        if (Handle.isError(b)) {
            runCatching { conn.releaseInterface(intf) }
            conn.close()
            throw NativeException((-b).toInt(), "USB 初期化に失敗: ${NativeBridge.nativeErrcName((-b).toInt())}")
        }
        this.conn = conn; this.intf = intf; this.bus = b
        lunCount = NativeBridge.nativeUsbBusLunCount(b).coerceAtLeast(1)
        android.util.Log.i("usbmem", "USB link [${device.deviceName}]: luns=$lunCount max-lun=${NativeBridge.nativeUsbBusMaxLunNote(b)}")
        opened = true
    }

    /** LUN 一覧 (skipMask の bit n が 1 の LUN は調べない)。接続を開いた後に呼ぶ。 */
    @Synchronized internal fun scan(skipMask: Int): List<UsbLunInfo> {
        check(opened && !shut) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeUsbBusScanLuns(bus, skipMask, 3000, err)
            ?: throw NativeException(err[0], "LUN 一覧の取得に失敗: ${NativeBridge.nativeErrcName(err[0])}")
        return UsbLunInfo.parseList(String(bytes, Charsets.UTF_8))
    }

    /** bus → インタフェース解放 → 接続。呼ぶのは、全セッションを閉じた後だけ (fd をネイティブが使い終えている)。 */
    private fun shutdown() {
        if (shut) return
        shut = true
        if (opened) {
            if (Handle.valid(bus)) NativeBridge.nativeUsbBusClose(bus)
            bus = 0
            runCatching { conn?.let { c -> intf?.let { c.releaseInterface(it) } } }
            runCatching { conn?.close() }
        }
        onShutdown(this)
    }
}

/**
 * 接続済みの USB マスストレージの LUN 1 つ。常に読み取り専用で開く (書き込みは検証済みになるまで有効化しない)。
 * close() は NTFS ボリュームを先に閉じてから、ネイティブデバイスを閉じ、接続の参照 ([UsbLink]) を手放す
 * (同じデバイスの他の LUN が開いていれば、インタフェースの解放・接続のクローズは最後の LUN が閉じるまで行わない)。
 */
class UsbStorageSession private constructor(
    override val deviceName: String,
    private val link: UsbLink,
    override val lun: Int,
    private var handle: Long,
    /** true = 書き込み可能で開いている (MSC が WRITE を送れる)。既定は読み取り専用。 */
    override val writable: Boolean = false,
    /** 保存した転送サイズの設定を使っている場合の LADDER の添字 (-1 = 既定)。マウント失敗時に、保存を捨てるかの判断に使う。 */
    override val tuningIndex: Int = -1,
    override val vendorId: Int = 0,
    override val productId: Int = 0,
    /** 開くときに 1 度だけ判別した媒体の種別 (回転ディスクか、フラッシュ/SSD か)。 */
    override val mediaKind: MediaKindInfo = MediaKindInfo.UNKNOWN,
) : UsbSession {
    private val volumes = mutableListOf<NtfsVolume>()

    /** セッション 1 本ごとのネイティブハンドル (usbmem_device*)。ネイティブ側にグローバルな状態は無く、すべてこのハンドルに閉じる。 */
    @Synchronized
    override fun <T> withHandle(block: (Long) -> T): T {
        check(Handle.valid(handle)) { "closed" }
        return block(handle)
    }

    @Synchronized
    override fun partitionTable(): PartitionTable {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(handle, err)
            ?: throw NativeException(err[0], "partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        return PartitionTable.parse(String(bytes, Charsets.UTF_8))
    }

    /** 診断用: これまでに送った SCSI オペコードの集計 (例: "12x1 1ax1 25x1 28x4237")。WRITE 系 (2a/8a) や 35 が出ないことの確認用。 */
    @Synchronized
    override fun opStats(): String = if (Handle.valid(handle)) NativeBridge.nativeUsbOpStats(handle) else ""

    /** 診断用: 直近の SCSI センス [key, asc, ascq]。 */
    fun lastSense(): IntArray = IntArray(3).also { if (Handle.valid(handle)) NativeBridge.nativeUsbLastSense(handle, it) }

    /** [createOnly] = true なら新規作成だけ許可 (既存の変更・削除・名前変更を拒否)。false なら NTFS の書き込み操作を全て許可。 */
    @Synchronized
    override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean): NtfsVolume {
        check(Handle.valid(handle)) { "closed" }
        if (writable && !this.writable) throw NativeException(30, "このセッションは読み取り専用で開かれています")
        val v = if (writable) NtfsVolume.mountDeviceRangeReadWrite(handle, offsetBytes, sizeBytes, createOnly) else NtfsVolume.mountDeviceRange(handle, offsetBytes, sizeBytes, allowNoload)
        return v.also { volumes += it }
    }

    /** ディスクの回転を止める (false) / 再開する (true)。SCSI START STOP UNIT。公開中のボリュームは、呼び出し側が先に閉じておくこと。未対応は NativeException(9)。 */
    @Synchronized
    override fun startStop(start: Boolean) {
        check(Handle.valid(handle)) { "closed" }
        val r = NativeBridge.nativeUsbStartStop(handle, start)
        if (r != 0) throw NativeException(-r, "start/stop failed: ${NativeBridge.nativeErrcName(-r)}")
    }

    /** 回転を止めている状態か (手動・自動どちらでも)。次の I/O で自動的に再開する。 */
    @Synchronized
    override fun isSpunDown(): Boolean = Handle.valid(handle) && NativeBridge.nativeUsbSpunDown(handle)

    private var idleWatcher: Thread? = null

    /**
     * アイドル自動スピンダウンの監視。最後の I/O から idleMinutes 分たったら止める (0 = 監視しない)。
     * 止めた後の読み書きは MscDevice が先に START を送るので、呼び出し側は何もしなくてよい。
     * 未対応のディスクは 1 度だけ試してあきらめる。onStopped は止めたとき (UI スレッドではない) に呼ぶ。
     */
    @Synchronized
    override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) {
        idleWatcher?.interrupt()
        idleWatcher = null
        if (idleMinutes <= 0) return
        val idleMs = idleMinutes * 60_000L
        val t = Thread {
            try {
                while (true) {
                    Thread.sleep(minOf(30_000L, maxOf(1_000L, idleMs / 4)))
                    val rc = synchronized(this) { if (Handle.valid(handle)) NativeBridge.nativeUsbIdleStop(handle, idleMs) else return@Thread }
                    if (rc == 1) android.util.Log.i("usbmem", "idle spin-down after ${idleMinutes} min")
                    if (rc >= 0) onStopped(isSpunDown())   // 止めた/再開した、どちらも UI に反映する
                    else { android.util.Log.w("usbmem", "idle spin-down not available: ${NativeBridge.nativeErrcName(-rc)}"); return@Thread }
                }
            } catch (_: InterruptedException) {}
        }
        t.isDaemon = true; t.name = "usbmem-idle-spindown"; t.start()
        idleWatcher = t
    }

    /** 転送サイズの調整 (速度計測用)。KiB。0 = 変更しない。 */
    @Synchronized
    override fun tune(chunkKiB: Int, maxTransferKiB: Int) {
        check(Handle.valid(handle)) { "closed" }
        val r = NativeBridge.nativeUsbTune(handle, chunkKiB, maxTransferKiB)
        if (r != 0) throw NativeException(-r, "tune failed")
    }

    /** 速度計測 + 読んだデータのハッシュ (転送サイズを変えても同じ内容が読めるかの確認用)。戻り値: 経過マイクロ秒、hash[0] = ハッシュ。 */
    @Synchronized
    override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray): Long {
        check(Handle.valid(handle)) { "closed" }
        val us = NativeBridge.nativeBenchReadHash(handle, offset, totalBytes, bufKiB, hash)
        if (us <= 0) throw NativeException((-us).toInt(), "bench failed: ${NativeBridge.nativeErrcName((-us).toInt())}")
        return us
    }

    /** 読み取り専用の速度計測: offset から totalBytes を bufKiB ごとに読み、経過マイクロ秒を返す。 */
    @Synchronized
    override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int): Long {
        check(Handle.valid(handle)) { "closed" }
        val us = NativeBridge.nativeBenchRead(handle, offset, totalBytes, bufKiB)
        if (us <= 0) throw NativeException((-us).toInt(), "bench failed: ${NativeBridge.nativeErrcName((-us).toInt())}")
        return us
    }

    private var closed = false

    @Synchronized
    override fun close() {
        idleWatcher?.interrupt(); idleWatcher = null
        volumes.forEach { runCatching { it.close() } }
        volumes.clear()
        if (Handle.valid(handle)) NativeBridge.nativeClose(handle)
        handle = 0
        if (!closed) { closed = true; link.sessionClosed() }   // 最後の LUN なら、ここでインタフェースを解放して接続を閉じる
    }

    companion object {
        const val ACTION_PERMISSION = "org.usbmem.app.USB_PERMISSION"
        /** 読み取りの既定の転送サイズ (KiB): USB 1 回の転送要求 / 1 SCSI READ コマンド (計測後の復帰先)。 */
        const val READ_CHUNK_KIB = 16
        const val READ_MAX_TRANSFER_KIB = 64

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

        /**
         * [osMountedUsbVolumes] のうち、**このデバイスを開くのを妨げるもの**。複数の USB を同時に使うとき、別の USB (まだ開いていない他のメモリ) を OS が
         * マウントしていても、それを理由にこのデバイスを拒否しない。ボリュームの持ち主を [ownerOf] (vold のノード名 -> 持ち主の deviceName) で調べ、
         * **持ち主がこのデバイス、または分からない (null) もの**だけを返す。分からないものは従来どおり安全側 (妨げる) に倒す。
         */
        fun osMountedBlocking(mountsText: String, deviceName: String, ownerOf: (String) -> String?): List<String> =
            osMountedUsbVolumes(mountsText).filter { v -> ownerOf(v).let { it == null || it == deviceName } }

        /**
         * sysfs の実パス ([blockPath]: /sys/dev/block/8:N の canonical path) から USB デバイスの deviceName ("/dev/bus/usb/BBB/DDD") を求める。
         * 親ディレクトリをたどって、最初に busnum と devnum を持つもの (= USB デバイスのディレクトリ。インタフェースやハブより下にある) を使う。分からなければ null。
         */
        fun usbOwnerOfSysfsPath(blockPath: java.io.File): String? {
            var f: java.io.File? = blockPath
            while (f != null) {
                val bus = java.io.File(f, "busnum"); val dev = java.io.File(f, "devnum")
                if (bus.isFile && dev.isFile) {
                    val b = bus.readText().trim().toIntOrNull() ?: return null
                    val d = dev.readText().trim().toIntOrNull() ?: return null
                    return "/dev/bus/usb/%03d/%03d".format(b, d)
                }
                f = f.parentFile
            }
            return null
        }

        /** vold のノード名 ("/dev/block/vold/public:8,17") の持ち主の deviceName。sysfs を読めない端末 (SELinux など) では null (= 分からない)。 */
        fun sysfsOwnerOfVoldNode(node: String): String? = runCatching {
            val mm = node.substringAfter("public:").split(',')
            val real = java.io.File("/sys/dev/block/${mm[0].toInt()}:${mm[1].toInt()}").canonicalFile
            if (!real.path.startsWith("/sys/")) null else usbOwnerOfSysfsPath(real)
        }.getOrNull()

        /**
         * 確保済みの接続 [link] の [lun] を開く (USB パーミッション取得済み。接続は [UsbLink.ensureOpen] 済み)。成功したら [link] の参照はセッションのものになる
         * (失敗時の参照は呼び出し側が [UsbLink.unref] で手放す)。
         */
        internal fun open(ctx: Context, link: UsbLink, device: UsbDevice, lun: Int, writable: Boolean): UsbStorageSession {
            // 初回の INQUIRY が EPROTO(71) で失敗することがある (J9260 で毎回)。インタフェースを確保したまま (カーネルの usb-storage を戻さず) 少し待って再試行する。
            // 解放して開き直すと OS が FAT を再マウントして、手動の取り出しが必要になる。開く時のコマンドは読み取り系だけ (INQUIRY/TUR/READ CAPACITY) なので、再試行しても安全。
            var h = NativeBridge.nativeOpenUsbMscLun(link.bus, lun, !writable)
            var attempt = 1
            while (Handle.isError(h) && attempt < 3) {
                android.util.Log.w("usbmem", "USB open attempt $attempt failed (lun $lun): ${NativeBridge.nativeErrcName((-h).toInt())} trace=${NativeBridge.nativeUsbLastOpenTrace()} -> retry")
                Thread.sleep(700)
                h = NativeBridge.nativeOpenUsbMscLun(link.bus, lun, !writable)
                attempt++
            }
            if (Handle.isError(h)) {
                val code = (-h).toInt()
                // 失敗の原因調査用 (SCSI コマンドと結果のみ。個人情報なし)
                android.util.Log.w("usbmem", "USB open failed (lun $lun): ${NativeBridge.nativeErrcName(code)} trace=${NativeBridge.nativeUsbLastOpenTrace()}")
                if (link.lunCount > 1 && NativeBridge.nativeErrcName(code) == "disconnected") throw NativeException(code, "LUN $lun にメディアがありません (カードを入れて「更新」を押してください)")
                throw NativeException(code, "USB 初期化に失敗: ${NativeBridge.nativeErrcName(code)}")
            }
            // 転送サイズは既定 (USB 16K / コマンド 64K)。「最適化」で、この端末・この USB 機器で実際に通った設定が保存されていれば、読み取りだけそれを使う
            // (自動では試さない: J9260 では 64K/256K で EPROTO になり、USB が切れて OS が FAT を再マウントした)。書き込みの転送サイズは変えない。
            val saved = TransferTuning.load(ctx, device.vendorId, device.productId)
            var tuningIndex = -1
            if (saved != null && runCatching { NativeBridge.nativeUsbTune(h, saved.first, saved.second) }.getOrDefault(-1) == 0) {
                tuningIndex = TransferTuning.LADDER.indexOf(saved)
                android.util.Log.i("usbmem", "saved read tuning applied: chunk=${saved.first}K cmd=${saved.second}K")
            }
            // 回転ディスクか、フラッシュ/SSD か (ネイティブが open の中で 1 度だけ判別済み。ここは結果を読むだけでコマンドは送らない)
            val kindVals = IntArray(6)
            val kindDetail = arrayOfNulls<String>(1)
            val mediaKind = if (NativeBridge.nativeUsbMediaInfo(h, kindVals, kindDetail) == 0) MediaKindInfo.fromNative(kindVals, kindDetail[0]) else MediaKindInfo.UNKNOWN
            android.util.Log.i("usbmem", "media kind [${device.deviceName} lun $lun]: ${mediaKind.kind} rpm=${mediaKind.rpm} basis=${mediaKind.basis} hint=${mediaKind.hint} :: ${mediaKind.detail}")
            link.markSession()
            return UsbStorageSession(device.deviceName, link, lun, h, writable, tuningIndex, device.vendorId, device.productId, mediaKind)
        }
    }
}

/**
 * 実機の [UsbManager] でデバイスの LUN を開く [UsbSessionFactory]。パーミッション取得済みのデバイスだけを開ける。
 * 同じデバイスの LUN は 1 つの [UsbLink] (接続 + 確保済みのインタフェース) を共有する。
 */
class UsbStorageSessionFactory(private val ctx: Context, private val usb: UsbManager) : UsbSessionFactory {
    private val lock = Any()
    private val links = HashMap<String, UsbLink>()

    /** デバイスの接続を 1 つ参照する (無ければ作る。物理的に開くのは [UsbLink.ensureOpen])。デバイスごとに別なので、他のデバイスを待たせない。 */
    private fun acquire(dev: UsbDevice): UsbLink {
        while (true) {
            val link = synchronized(lock) { links.getOrPut(dev.deviceName) { UsbLink(dev.deviceName, usb, true) { l -> synchronized(lock) { if (links[l.deviceName] === l) links.remove(l.deviceName) } } } }
            if (link.tryRef()) return link
            synchronized(lock) { if (links[dev.deviceName] === link) links.remove(dev.deviceName) }   // 後始末済みの接続: 作り直す
        }
    }

    private fun deviceOf(info: UsbDeviceInfo): UsbDevice = usb.deviceList[info.deviceName] ?: throw NativeException(2, "USB デバイスが見つかりません (抜かれました)")

    override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession = open(device, 0, writable)

    override fun open(device: UsbDeviceInfo, lun: Int, writable: Boolean): UsbSession {
        val dev = deviceOf(device)
        val link = acquire(dev)
        try {
            link.ensureOpen(dev)
            if (lun >= link.lunCount) throw NativeException(22, "LUN $lun はありません (LUN 数 ${link.lunCount})")
            return UsbStorageSession.open(ctx, link, dev, lun, writable)
        } catch (t: Throwable) {
            link.unref()
            throw t
        }
    }

    override fun scanLuns(device: UsbDeviceInfo, skipMask: Int): UsbLunScan {
        val dev = deviceOf(device)
        val link = acquire(dev)
        try {
            link.ensureOpen(dev)
            val n = link.lunCount
            val scan = if (n <= 1) UsbLunScan(1) else UsbLunScan(n, link.scan(skipMask))
            link.keepForScan()   // 続けて開くまで (または releaseDevice まで) インタフェースを OS に返さない
            return scan
        } catch (t: Throwable) {
            link.unref()
            throw t
        }
    }

    override fun releaseDevice(deviceName: String) {
        val l = synchronized(lock) { links[deviceName] }
        l?.releaseHold()
    }
}
