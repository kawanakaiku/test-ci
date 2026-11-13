package org.usbmem.app

import org.json.JSONArray

/**
 * 複数 LUN (カードリーダーの SD スロット / microSD スロットなど) の「スロット名」。docs/multi-usb.md の LUN の節。
 *
 * スロット = (deviceName, lun)。1 つのスロットが 1 つのセッション・書き込みモード・アイドル設定・公開ボリュームの持ち主になる。
 * 文字列では `deviceName` (LUN 0。従来と同じ文字列なので、LUN 1 つの機器は従来のキーと互換) または `deviceName/l<lun>` (LUN 1 以上)。
 * ボリュームキーは `usb:<スロット名>@<開始バイト>` (`VolumeRegistry.usbKey`) なので、`usb:/dev/bus/usb/001/005/l1@0` のようになる。
 */
object UsbSlotKey {
    private val LUN_SUFFIX = Regex("""^(.*)/l(\d{1,2})$""")

    fun of(deviceName: String, lun: Int): String = if (lun <= 0) deviceName else "$deviceName/l$lun"

    /** スロット名から物理デバイス名 (UsbDevice.deviceName) を取り出す。LUN の接尾辞が無ければそのまま。 */
    fun deviceOf(slot: String): String = LUN_SUFFIX.matchEntire(slot)?.groupValues?.get(1) ?: slot

    fun lunOf(slot: String): Int = LUN_SUFFIX.matchEntire(slot)?.groupValues?.get(2)?.toInt() ?: 0
}

/** LUN の状態 (ネイティブの LunState と同じ名前)。 */
enum class UsbLunState {
    /** メディアあり・容量が分かった (開ける) */
    Ready,
    /** メディアなし (カードが入っていない) */
    NoMedia,
    /** 準備中のまま待ち時間を超えた */
    NotReady,
    /** 直接アクセス型ディスクではない、または LUN が接続されていない */
    Unsupported,
    /** コマンドが失敗した */
    Error,
    /** 調べなかった (すでに開いている LUN に余計なコマンドを送らないため) */
    Skipped;

    companion object {
        fun fromNative(name: String): UsbLunState = when (name) {
            "ready" -> Ready
            "no-media" -> NoMedia
            "not-ready" -> NotReady
            "unsupported" -> Unsupported
            "skipped" -> Skipped
            else -> Error
        }
    }
}

/** LUN 1 つの情報 (LUN 一覧の 1 行)。 */
data class UsbLunInfo(
    val lun: Int,
    val state: UsbLunState,
    val vendor: String = "",
    val product: String = "",
    val removable: Boolean = false,
    /** state == Ready のときの容量 (バイト) */
    val sizeBytes: Long = 0,
    val blockSize: Int = 0,
    val note: String = "",
) {
    /** 開ける LUN か (メディアがあって、容量が分かっている) */
    val openable: Boolean get() = state == UsbLunState.Ready

    companion object {
        /** `usbmem_usb_bus_scan_luns` の JSON。blocks は 64 ビット精度のため文字列。 */
        fun parseList(json: String): List<UsbLunInfo> {
            val a = JSONArray(json)
            return List(a.length()) { i ->
                val o = a.getJSONObject(i)
                val bs = o.optInt("blockSize", 0)
                val blocks = o.optString("blocks", "0").toULongOrNull()?.toLong() ?: 0L
                UsbLunInfo(
                    lun = o.getInt("lun"),
                    state = UsbLunState.fromNative(o.optString("state")),
                    vendor = o.optString("vendor"),
                    product = o.optString("product"),
                    removable = o.optBoolean("removable"),
                    sizeBytes = if (blocks > 0 && bs > 0 && blocks <= Long.MAX_VALUE / bs) blocks * bs else 0L,
                    blockSize = bs,
                    note = o.optString("note"),
                )
            }
        }
    }
}

/**
 * LUN の調査結果。[lunCount] = GET MAX LUN で分かった LUN 数 (応答しない・STALL・不正な値の機器は 1)。
 * [luns] は LUN 数が 2 以上のときだけ入る (LUN 1 つの機器には INQUIRY などを余分に送らず、そのまま LUN 0 を開く)。
 */
data class UsbLunScan(val lunCount: Int, val luns: List<UsbLunInfo> = emptyList()) {
    val multi: Boolean get() = lunCount >= 2
    /** メディアがある (開ける) LUN */
    val ready: List<UsbLunInfo> get() = luns.filter { it.openable }
}
