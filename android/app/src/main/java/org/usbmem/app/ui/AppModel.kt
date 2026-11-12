package org.usbmem.app.ui

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.usbmem.app.PartitionTable

/** 接続されている USB マスストレージ (一覧表示用)。device は MainActivity 側が持つ UsbDevice を引くためのキー。 */
data class UsbDeviceRow(val key: String, val title: String, val vidPid: String)

/** 公開 (SAF) 中のボリューム。 */
/** USB を開く前に決める書き込みモード。 */
enum class WriteMode { ReadOnly, CreateOnly, Full }

/** ext3/ext4 のジャーナル未再生 (EUCLEAN) で拒否したパーティション。ユーザーが確認すれば、読み取り専用で開き直せる。 */
data class NoloadPrompt(val startBytes: Long, val sizeBytes: Long, val label: String)

data class PublishedVolume(val label: String, val modeText: String, val writable: Boolean, val fullWrite: Boolean = false, val canLink: Boolean = false)

/**
 * 画面が読む状態をまとめたもの (Compose の状態保持)。ロジックは MainActivity が持ち、ここは値を更新するだけ。
 * 機能を足すときは、ここに状態を足し、対応する画面 (ui パッケージの各 Screen) に表示を足す。
 */
class AppModel {
    // ---- USB
    var usbDevices by mutableStateOf<List<UsbDeviceRow>>(emptyList())
    var osMountedUsb by mutableIntStateOf(0)                 // OS がマウント中の USB ボリューム数 (0 でないと、開く前に取り出しが必要)
    var writeMode by mutableStateOf(WriteMode.ReadOnly)      // USB を開く時の書き込みモード。毎回の起動で明示的に選び、保存しない (既定は読み取り専用)
    val allowUsbWrite: Boolean get() = writeMode != WriteMode.ReadOnly
    var usbOpenKey by mutableStateOf<String?>(null)          // 開いているデバイスのキー (null = 未接続)
    var usbWritable by mutableStateOf(false)                 // 開いているセッションが書き込み可か
    var usbLabel by mutableStateOf("")
    var table by mutableStateOf<PartitionTable?>(null)
    var published by mutableStateOf<PublishedVolume?>(null)
    var idleSpinDownMin by mutableStateOf(0)                 // アイドル自動スピンダウン (分、0 = オフ)。設定として保存する
    var diskIdleStopped by mutableStateOf(false)             // アイドルで止まっている (次のアクセスで自動再開)
    var diskStopped by mutableStateOf(false)                 // ディスクの回転を止めた状態 (再開するまで公開などはできない)
    var busy by mutableStateOf<String?>(null)                // 処理中の説明 (null = なし)
    var usbNotice by mutableStateOf<Notice?>(null)
    var noloadPrompt by mutableStateOf<NoloadPrompt?>(null)   // 非 null のとき、確認ダイアログを出す

    // ---- 診断
    var ops by mutableStateOf("")                            // 送信した SCSI 命令の集計
    var tuneLog by mutableStateOf("")                        // 速度計測 / 最適化の結果
    var selfTestLog by mutableStateOf("")                    // 書き込み自己診断の結果
    var selfTestPassed by mutableStateOf<Boolean?>(null)     // null = 未実施
    var tuningSaved by mutableStateOf<String?>(null)    // 保存した転送設定の表示 (null = 既定)
}
