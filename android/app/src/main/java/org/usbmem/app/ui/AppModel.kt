package org.usbmem.app.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.usbmem.app.MediaKindInfo
import org.usbmem.app.PartitionTable
import org.usbmem.app.UsbLunInfo
import org.usbmem.app.UsbSlotKey
import org.usbmem.app.WriteMode

/** ext3/ext4 のジャーナル未再生 (EUCLEAN) で拒否したパーティション。ユーザーが確認すれば、読み取り専用で開き直せる。 */
data class NoloadPrompt(val startBytes: Long, val sizeBytes: Long, val label: String)

/**
 * 公開 (SAF) 中のボリューム。[volumeKey] = VolumeRegistry の登録キー、[startBytes] = デバイス上の開始位置
 * (パーティション一覧のどの行かを引くキー。デバイス全体のマウントは 0)。
 */
data class PublishedVolume(
    val label: String, val modeText: String, val writable: Boolean, val fullWrite: Boolean = false, val canLink: Boolean = false,
    val volumeKey: String = "", val startBytes: Long = 0,
)

/**
 * USB のスロット 1 つ分の画面の状態 (Compose の状態保持)。スロット名 ([UsbSlotKey.of]: LUN 0 は UsbDevice.deviceName そのもの) をキーに、接続している間ずっと同じオブジェクトを使う。
 * LUN が 1 つの機器は 1 本 = 1 スロット。複数 LUN のカードリーダーは、LUN ごとに 1 スロット (= 1 枚のカード)。
 * 状態の持ち主はプロセス側の UsbSessionManager で、これはその写し。画面は読むだけ、更新は MainActivity (メインスレッド)。
 */
class UsbDeviceState(val key: String) {
    /** 物理デバイス名 (UsbDevice.deviceName)。OS のマウント検査・抜去はこの単位 */
    val deviceName: String get() = UsbSlotKey.deviceOf(key)
    /** このスロットの LUN (論理ユニット) */
    val lun: Int get() = UsbSlotKey.lunOf(key)
    /** 非 null = このデバイスは複数 LUN で、このカードは LUN [lun] (メディアの有無・容量はこの値)。null = LUN 1 つ、またはまだ調べていないデバイス */
    var lunInfo by mutableStateOf<UsbLunInfo?>(null)
    var lunCount by mutableIntStateOf(1)
    /** 保存する設定 (アイドル停止・スピンダウンの確認) のキー: 機種 (VID:PID)。複数 LUN では LUN ごと ("0781:5581/l1") */
    var settingsId by mutableStateOf("")
    var title by mutableStateOf("")                          // 機種名 + 識別子 (例: "SanDisk Ultra [A1B2]")
    var vidPid by mutableStateOf("")
    var shortId by mutableStateOf("")                        // シリアル下 4 桁 / バス番号-デバイス番号
    var productName by mutableStateOf("")
    var writeMode by mutableStateOf(WriteMode.ReadOnly)      // このデバイスを開く時の書き込みモード。毎回の起動で明示的に選び、保存しない (既定は読み取り専用)
    var open by mutableStateOf(false)                        // 開いている (= 接続中)
    var writable by mutableStateOf(false)                    // 開いているセッションが書き込み可か
    var table by mutableStateOf<PartitionTable?>(null)
    var published by mutableStateOf<List<PublishedVolume>>(emptyList())
    var osBlocked by mutableIntStateOf(0)                    // このデバイスを開くのを妨げる OS のマウント数 (0 でないと、開く前に取り出しが必要)
    var idleSpinDownMin by mutableIntStateOf(0)              // アイドル自動スピンダウン (分、0 = オフ)。このデバイスだけの設定
    var diskIdleStopped by mutableStateOf(false)             // アイドルで止まっている (次のアクセスで自動再開)
    var mediaKind by mutableStateOf(MediaKindInfo.UNKNOWN)   // 回転ディスクか、フラッシュ/SSD か (開くとき 1 度だけ判別)。開いている間だけ有効
    var spinDownUnlocked by mutableStateOf(false)            // スピンダウンの操作を出してよい: 回転ディスク、または回転ディスク以外でユーザーが確認済み
    var diskStopped by mutableStateOf(false)                 // ディスクの回転を止めた状態 (再開するまで公開などはできない)
    var busy by mutableStateOf<String?>(null)                // このデバイスの処理中の説明 (null = なし)
    var notice by mutableStateOf<Notice?>(null)
    var noloadPrompt by mutableStateOf<NoloadPrompt?>(null)   // 非 null のとき、確認ダイアログを出す

    // ---- 診断 (デバイスごと)
    var ops by mutableStateOf("")                            // 送信した SCSI 命令の集計
    var tuneLog by mutableStateOf("")                        // 速度計測 / 最適化の結果
    var selfTestLog by mutableStateOf("")                    // 書き込み自己診断の結果
    var selfTestPassed by mutableStateOf<Boolean?>(null)     // null = 未実施
    var tuningSaved by mutableStateOf<String?>(null)         // 保存した転送設定の表示 (null = 既定)

    /** 閉じたときに戻す項目 (接続中の状態だけ。モード・アイドル設定・通知は残す)。 */
    fun resetAfterClose() {
        open = false; writable = false; table = null; published = emptyList(); diskStopped = false; diskIdleStopped = false
        mediaKind = MediaKindInfo.UNKNOWN; spinDownUnlocked = false
        busy = null; noloadPrompt = null; ops = ""; tuneLog = ""; tuningSaved = null; selfTestLog = ""; selfTestPassed = null
    }
}

/**
 * 画面が読む状態をまとめたもの (Compose の状態保持)。ロジックは MainActivity が持ち、ここは値を更新するだけ。
 * プロセスで 1 つ ([shared]): Activity が作り直されても、開いている USB の表示が消えない (セッション自体もプロセス側が持つ)。
 * 機能を足すときは、ここに状態を足し、対応する画面 (ui パッケージの各 Screen) に表示を足す。
 */
class AppModel {
    companion object {
        val shared: AppModel by lazy { AppModel() }
    }

    // ---- USB (デバイスごとの状態は UsbDeviceState)
    var usbDevices by mutableStateOf<List<UsbDeviceState>>(emptyList())
    var usbNotice by mutableStateOf<Notice?>(null)           // どのデバイスにも属さない/抜かれて画面から消えたデバイスの通知 (本文にデバイス名を含める)

    fun device(key: String): UsbDeviceState? = usbDevices.firstOrNull { it.key == key }
}
