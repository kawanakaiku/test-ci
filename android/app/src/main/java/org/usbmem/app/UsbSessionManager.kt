package org.usbmem.app

import java.util.concurrent.CopyOnWriteArrayList

/**
 * USB を開く時の書き込みモード (デバイスごと)。既定は読み取り専用。開いている間は変えられない。
 * [Full] は既存ファイルの変更・削除も許す (実験的)。docs/multi-usb.md
 */
enum class WriteMode { ReadOnly, CreateOnly, Full }

/** 接続中の USB マスストレージの、Android に依存しない最小の記述 (JVM 単体テストで偽物を作れるように)。 */
data class UsbDeviceInfo(
    /** UsbDevice.deviceName ("/dev/bus/usb/001/005")。セッション・ボリューム・画面の状態のキー。 */
    val deviceName: String,
    val productName: String,
    val vendorId: Int,
    val productId: Int,
    /** シリアル番号。権限を得る前、または読めない機器では null。 */
    val serial: String? = null,
)

/**
 * 開いた USB デバイス 1 本分の操作面。実体は [UsbStorageSession]。マネージャと画面はこのインタフェースだけを使い、
 * テストでは偽物に差し替える。すべてのメソッドはスレッドセーフで、close 後に呼ぶと IllegalStateException。
 */
interface UsbSession : AutoCloseable {
    val deviceName: String
    /** true = 書き込み可能で開いている (MSC が WRITE を送れる)。 */
    val writable: Boolean
    val vendorId: Int
    val productId: Int
    /** 保存した転送設定を使っている場合の LADDER の添字 (-1 = 既定)。 */
    val tuningIndex: Int

    fun partitionTable(): PartitionTable
    fun opStats(): String
    fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean = false, createOnly: Boolean = true, allowNoload: Boolean = false): NtfsVolume
    fun startStop(start: Boolean)
    fun isSpunDown(): Boolean
    fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit = {})
    fun tune(chunkKiB: Int, maxTransferKiB: Int)
    fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray): Long
    fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int): Long

    /**
     * このデバイスのネイティブハンドルを、開いている間だけ渡す (検査・mkfs・バックアップ・パーティション編集などの DiskTools / RecoveryBackup / PartitionEditor 用)。
     * セッションのロックを持つので、ブロック内の処理中は close されない。デバイスごとのハンドルなので、他のデバイスには影響しない。
     */
    fun <T> withHandle(block: (Long) -> T): T
}

/** デバイスを開いて [UsbSession] にする。実機では [UsbStorageSessionFactory]、テストでは偽物。失敗は例外 (NativeException など)。 */
fun interface UsbSessionFactory {
    fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession
}

enum class CloseReason { User, Detached, Shutdown }

/** マネージャの拒否。[kind] で画面の文言を選ぶ。 */
class UsbSessionException(val kind: Kind, message: String) : Exception(message) {
    enum class Kind {
        /** 開いている (または開いている最中)。二重オープンの拒否 */
        AlreadyOpen,
        /** 閉じている最中。終わってから開き直す */
        Closing,
        /** 同時に開ける数の上限 */
        LimitReached,
        /** このビルドは書き込みが無効 (書き込みモードを選べない) */
        WriteNotAllowed,
        /** 開いている最中にデバイスが抜かれた (または閉じられた) */
        ClosedWhileOpening,
    }
}

/** セッションの変化の通知。呼び出し元のスレッドで、マネージャのロックの外から呼ぶ (UI はメインスレッドへ渡すこと)。 */
interface UsbSessionListener {
    fun onOpened(deviceName: String) {}
    fun onClosed(deviceName: String, reason: CloseReason) {}
    /** アイドル自動スピンダウンで止まった/再開した (監視スレッドから)。 */
    fun onIdleChanged(deviceName: String, stopped: Boolean) {}
}

/**
 * 複数の USB デバイスを同時に開いて、デバイス名をキーに 1 本ずつ管理する。docs/multi-usb.md
 *
 * - デバイスごとに独立: 開く・閉じる・抜去・書き込みモード・アイドル停止の設定は、他のデバイスに影響しない。
 * - 同じデバイスの二重オープンは拒否する ([UsbSessionException.Kind.AlreadyOpen])。開く最中・閉じる最中も状態として持つ。
 * - 遅い処理 (USB を開く・書き込みの確定・クローズ) は**ロックの外**で行う。ロックは状態の読み書きだけを守るので、1 本の遅い処理が他の本を止めない。
 * - 同時に開ける数は [maxOpen] (既定は無制限)。USB の帯域・給電は物理的な制約で、画面で注意を出す。
 *
 * @param writeSupported false = このビルドは書き込みが無効。書き込みモードを選べず、開くと ReadOnly 以外は拒否する。
 * @param onDeviceClosing セッションを閉じる直前に呼ぶ、そのデバイスの公開中ボリューム (ネストした子を含む) の後始末。
 */
class UsbSessionManager(
    private val factory: UsbSessionFactory,
    val writeSupported: Boolean = true,
    private val maxOpen: Int = Int.MAX_VALUE,
    private val onDeviceClosing: (deviceName: String) -> Unit = {},
) {
    private enum class Phase { Opening, Open, Closing }

    private class Slot(var phase: Phase, val mode: WriteMode) {
        var session: UsbSession? = null
        /** 開く最中に close / 抜去が来たときの理由。開き終わった直後に閉じる。 */
        var pendingClose: CloseReason? = null
    }

    private val lock = Any()
    private val slots = HashMap<String, Slot>()
    private val modes = HashMap<String, WriteMode>()
    private val idleMinutes = HashMap<String, Int>()
    private val listeners = CopyOnWriteArrayList<UsbSessionListener>()

    fun addListener(l: UsbSessionListener) { listeners.addIfAbsent(l) }
    fun removeListener(l: UsbSessionListener) { listeners.remove(l) }

    // ---- 状態の参照

    /** 開き終えている (操作できる) か。開く最中・閉じる最中は false。 */
    fun isOpen(deviceName: String): Boolean = synchronized(lock) { slots[deviceName]?.phase == Phase.Open }

    /** 開く最中、開いている、閉じる最中のいずれか (= このデバイスの設定を変えてはいけない)。 */
    fun isBusy(deviceName: String): Boolean = synchronized(lock) { slots.containsKey(deviceName) }

    /** 開いているセッション。開き終えていなければ null。 */
    fun session(deviceName: String): UsbSession? = synchronized(lock) { slots[deviceName]?.takeIf { it.phase == Phase.Open }?.session }

    /** 開き終えているデバイス名の一覧 (開いた順ではない)。 */
    fun openDevices(): List<String> = synchronized(lock) { slots.filterValues { it.phase == Phase.Open }.keys.sorted() }

    fun openCount(): Int = synchronized(lock) { slots.size }

    /** そのデバイスの書き込みモード。開いている間は、開いた時のモード。 */
    fun writeMode(deviceName: String): WriteMode = synchronized(lock) { slots[deviceName]?.mode ?: modes[deviceName] ?: WriteMode.ReadOnly }

    // ---- 設定 (デバイスごと)

    /**
     * 書き込みモードを変える。そのデバイスを開く前 (開く最中・閉じる最中も不可) だけ可能で、変えられたら true。
     * ビルドで書き込みが無効なら ReadOnly 以外は選べない。
     */
    fun setWriteMode(deviceName: String, mode: WriteMode): Boolean = synchronized(lock) {
        if (slots.containsKey(deviceName)) return false
        if (mode != WriteMode.ReadOnly && !writeSupported) return false
        modes[deviceName] = mode
        true
    }

    fun idleSpinDownMinutes(deviceName: String): Int = synchronized(lock) { idleMinutes[deviceName] ?: 0 }

    /** アイドル自動スピンダウンの分数 (0 = オフ)。そのデバイスだけに効く。開いていれば直ちに反映し、次に開くときにも使う。 */
    fun setIdleSpinDownMinutes(deviceName: String, minutes: Int) {
        val s = synchronized(lock) { idleMinutes[deviceName] = minutes; slots[deviceName]?.takeIf { it.phase == Phase.Open }?.session }
        s?.let { applyIdle(deviceName, it, minutes) }
    }

    private fun applyIdle(deviceName: String, s: UsbSession, minutes: Int) {
        s.setIdleSpinDown(minutes) { stopped -> listeners.forEach { it.onIdleChanged(deviceName, stopped) } }
    }

    // ---- 開く・閉じる

    /**
     * [device] を、そのデバイスの書き込みモードで開く。USB の初期化に数秒かかるので、UI スレッドでは呼ばないこと。
     * 既に開いている/開いている最中なら [UsbSessionException.Kind.AlreadyOpen]。他のデバイスが開いていても影響しない。
     * 開く最中にデバイスが抜かれたら ClosedWhileOpening (開いたセッションは閉じて返さない)。
     */
    fun open(device: UsbDeviceInfo): UsbSession {
        val name = device.deviceName
        val slot: Slot
        synchronized(lock) {
            slots[name]?.let { throw UsbSessionException(if (it.phase == Phase.Closing) UsbSessionException.Kind.Closing else UsbSessionException.Kind.AlreadyOpen, "already open: $name") }
            if (slots.size >= maxOpen) throw UsbSessionException(UsbSessionException.Kind.LimitReached, "too many open devices: $maxOpen")
            val mode = modes[name] ?: WriteMode.ReadOnly
            if (mode != WriteMode.ReadOnly && !writeSupported) throw UsbSessionException(UsbSessionException.Kind.WriteNotAllowed, "write is disabled in this build")
            slot = Slot(Phase.Opening, mode).also { slots[name] = it }
        }
        val session = try {
            factory.open(device, slot.mode != WriteMode.ReadOnly)
        } catch (t: Throwable) {
            synchronized(lock) { if (slots[name] === slot) slots.remove(name) }
            throw t
        }
        val (pending, idle) = synchronized(lock) {
            slot.session = session
            // 開く最中に閉じろと言われていたら、Open にせず同じロックの中で Closing にする (他のスレッドが割り込んで二重に閉じないように)
            slot.phase = if (slot.pendingClose != null) Phase.Closing else Phase.Open
            slot.pendingClose to (idleMinutes[name] ?: 0)
        }
        if (pending != null) {   // 開いている最中に抜かれた/閉じろと言われた
            finishClose(name, slot, pending)
            throw UsbSessionException(UsbSessionException.Kind.ClosedWhileOpening, "closed while opening: $name")
        }
        if (idle > 0) applyIdle(name, session, idle)
        listeners.forEach { it.onOpened(name) }
        return session
    }

    /**
     * そのデバイスのセッションを閉じる (公開中のボリュームの後始末 → 書き込みの確定 → USB の解放)。他のデバイスには触れない。
     * 閉じる処理を始めたら true。開いていない・既に閉じている最中なら false (開く最中なら、開き終えた直後に閉じる)。
     * 戻る時点でそのデバイスは閉じ終わっている (true のとき)。
     */
    fun close(deviceName: String, reason: CloseReason = CloseReason.User): Boolean {
        val slot = synchronized(lock) {
            val s = slots[deviceName] ?: return false
            when (s.phase) {
                Phase.Opening -> { s.pendingClose = reason; return false }
                Phase.Closing -> return false
                Phase.Open -> { s.phase = Phase.Closing; s }
            }
        }
        finishClose(deviceName, slot, reason)
        return true
    }

    private fun finishClose(name: String, slot: Slot, reason: CloseReason) {
        synchronized(lock) { slot.phase = Phase.Closing }
        try {
            runCatching { onDeviceClosing(name) }        // 公開中のボリューム (ネストした子も) を閉じて SAF のルートを消す
            runCatching { slot.session?.close() }
        } finally {
            synchronized(lock) {
                if (slots[name] === slot) slots.remove(name)
                if (reason == CloseReason.Detached) { modes.remove(name); idleMinutes.remove(name) }   // 抜かれた機器の設定は引き継がない
            }
        }
        listeners.forEach { it.onClosed(name, reason) }
    }

    /**
     * デバイスが抜かれた。そのデバイスのセッションだけを閉じ、そのデバイスの書き込みモードなどの設定は捨てる (次に挿したときは読み取り専用から)。
     * 抜かれたデバイスへの I/O はすでに失敗するが、後始末 (ボリュームを閉じる) は必ず行う。
     */
    fun onDetached(deviceName: String): Boolean {
        val closed = close(deviceName, CloseReason.Detached)
        if (!closed) synchronized(lock) { if (!slots.containsKey(deviceName)) { modes.remove(deviceName); idleMinutes.remove(deviceName) } }   // 開いていなかった機器の設定
        return closed
    }

    /** 全部閉じる (アプリの終了時)。1 本の失敗で他を止めない。 */
    fun closeAll(reason: CloseReason = CloseReason.Shutdown) {
        for (n in openDevices()) runCatching { close(n, reason) }
    }
}

/**
 * 画面とボリュームの公開名に付けるデバイスの識別子。同じ機種を何本も挿しても、公開名 (SAF のルート名) がぶつからないようにする。
 * 「機種名 [シリアル下 4 桁]」。シリアルが無い/他と重なる (安価な USB メモリは同じ値を返すことがある) ときは、バス番号-デバイス番号 ("1-5")。
 */
object UsbDeviceNaming {
    private const val DEFAULT_NAME = "USB"

    /** バス番号-デバイス番号 ("/dev/bus/usb/001/005" -> "1-5")。読めなければ deviceName のハッシュ 4 桁。同時に接続している間は一意。 */
    fun busId(deviceName: String): String {
        val m = Regex("""/(\d+)/(\d+)$""").find(deviceName)
        if (m != null) return "${m.groupValues[1].toInt()}-${m.groupValues[2].toInt()}"
        return "%04x".format(deviceName.hashCode() and 0xffff)
    }

    private fun serialTail(serial: String?): String? {
        val s = serial?.filter { it.isLetterOrDigit() }?.uppercase().orEmpty()
        if (s.length < 4 || s.all { it == '0' }) return null   // 短い/全部 0 は識別にならない
        return s.takeLast(4)
    }

    /** 接続中の全デバイスの識別子 (deviceName -> id)。全員が互いに異なることを保証する。 */
    fun shortIds(devices: List<UsbDeviceInfo>): Map<String, String> {
        val preferred = devices.associate { it.deviceName to (serialTail(it.serial) ?: busId(it.deviceName)) }
        val count = preferred.values.groupingBy { it }.eachCount()
        val out = LinkedHashMap<String, String>()
        for (d in devices) {
            val p = preferred.getValue(d.deviceName)
            out[d.deviceName] = if (count.getValue(p) == 1) p else busId(d.deviceName)   // 重なったものはバス番号に切り替える (バス番号は一意)
        }
        // バス番号がシリアル下 4 桁と偶然同じになる可能性は無い ("-" を含む) が、ハッシュ形式の重なりに備えて最後に確認する
        val seen = HashSet<String>()
        for ((k, v) in out.entries.toList()) {
            var id = v; var n = 2
            while (!seen.add(id)) id = "$v-${n++}"
            out[k] = id
        }
        return out
    }

    fun displayName(productName: String?, shortId: String): String = "${productName?.takeIf { it.isNotBlank() } ?: DEFAULT_NAME} [$shortId]"

    /** SAF のルート名になるボリューム名: 「機種名 [識別子] #パーティション番号」(パーティション表が無い全体は番号なし)。 */
    fun volumeLabel(productName: String?, shortId: String, partitionIndex: Int?): String =
        displayName(productName, shortId) + (if (partitionIndex != null) " #$partitionIndex" else "")
}
