package org.usbmem.app

import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class UsbRow(val device: android.hardware.usb.UsbDevice, val title: String)

sealed interface UiState {
    data object Idle : UiState
    data class Loaded(val label: String, val table: PartitionTable) : UiState
    data class Failed(val message: String) : UiState
}

class MainActivity : ComponentActivity() {
    private val usbManager by lazy { getSystemService(USB_SERVICE) as android.hardware.usb.UsbManager }
    private var usbSession: UsbStorageSession? = null
    private var usbState by mutableStateOf<UiState>(UiState.Idle)
    private var usbRows by mutableStateOf<List<UsbRow>>(emptyList())
    private var allowUsbWrite by mutableStateOf(false)   // USB を開く時の書き込み許可 (新規作成のみ)。毎回の起動で明示的に許可し、保存しない
    private var usbBench by mutableStateOf("")           // 速度計測の結果 (読み取りのみ)
    private var usbOps by mutableStateOf("")   // 送信した SCSI 命令の集計 (読み取り専用で書き込み系が出ていないことの確認用)

    private fun updateOps(tag: String) {
        val o = usbSession?.opStats() ?: return
        android.util.Log.i("usbmem", "SCSI ops [$tag]: $o")
        runOnUiThread { usbOps = o }
    }

    private val usbReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(c: android.content.Context, i: android.content.Intent) {
            val dev: android.hardware.usb.UsbDevice? =
                if (android.os.Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(android.hardware.usb.UsbManager.EXTRA_DEVICE, android.hardware.usb.UsbDevice::class.java)
                else @Suppress("DEPRECATION") i.getParcelableExtra(android.hardware.usb.UsbManager.EXTRA_DEVICE)
            when (i.action) {
                UsbStorageSession.ACTION_PERMISSION ->
                    if (dev != null && i.getBooleanExtra(android.hardware.usb.UsbManager.EXTRA_PERMISSION_GRANTED, false)) openUsb(dev)
                    else usbState = UiState.Failed("USB の許可が得られませんでした")
                android.hardware.usb.UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    // 抜去: 公開中のボリュームと接続を直ちに閉じる (抜けた後の I/O は Disconnected になるが、後始末を先に行う)
                    if (dev != null && dev.deviceName == usbSession?.deviceName) closeUsb("USB デバイスが取り外されました")
                }
                android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED -> refreshUsb()
            }
        }
    }

    private fun refreshUsb() {
        usbRows = usbManager.deviceList.values
            .filter { UsbMscSelector.pick(UsbMscSelector.describe(it)) != null }
            .map { UsbRow(it, "${it.productName ?: "USB ストレージ"} (${String.format("%04x:%04x", it.vendorId, it.productId)})") }
    }

    private fun requestUsb(dev: android.hardware.usb.UsbDevice) {
        if (usbManager.hasPermission(dev)) openUsb(dev) else usbManager.requestPermission(dev, UsbStorageSession.permissionIntent(this))
    }

    private fun openUsb(dev: android.hardware.usb.UsbDevice) {
        Thread {
            val st = try {
                closeUsbQuiet()
                val s = UsbStorageSession.open(this, usbManager, dev, writable = allowUsbWrite)
                usbSession = s
                val loaded = UiState.Loaded(dev.productName ?: "USB", s.partitionTable())
                updateOps("after open+partition scan")
                loaded
            } catch (e: NativeException) {
                UiState.Failed(e.message ?: "native error")
            } catch (e: Exception) {
                UiState.Failed(e.javaClass.simpleName)
            }
            runOnUiThread { usbState = st }
        }.start()
    }

    /** 読み取り専用の速度計測。転送サイズの組を順に試し、MB/s を表示・ログへ出す。パーティションの先頭付近 (デバイス先頭から) を読むだけで、何も書かない。 */
    private fun benchUsb() {
        Thread {
            val s = usbSession ?: return@Thread
            val sb = StringBuilder()
            try {
                val total = 128L shl 20
                // (USB 1 回の転送要求 KiB, 1 SCSI コマンド KiB)。0,0 = 既定のまま。区間はずらして、デバイス側のキャッシュ効果を避ける
                val configs = listOf(0 to 0, 64 to 256, 256 to 512, 512 to 1024, 1024 to 2048)
                configs.forEachIndexed { i, (chunk, mt) ->
                    val off = (1L shl 30) + i * (256L shl 20)
                    val line = try {
                        if (chunk != 0 || mt != 0) s.tune(chunk, mt)
                        val us = s.benchRead(off, total, 1024)
                        "chunk=${if (chunk == 0) "既定" else "${chunk}K"} cmd=${if (mt == 0) "既定" else "${mt}K"}: %.1f MB/s".format(total / 1048576.0 / (us / 1e6))
                    } catch (e: NativeException) { "chunk=${chunk}K cmd=${mt}K: 失敗 ${e.message}" }
                    android.util.Log.i("usbmem", "bench $line"); sb.appendLine(line)
                }
                s.tune(16, 64)   // 既定へ戻す (USB 転送要求 16KiB / 1 コマンド 64KiB)
            } catch (e: Exception) { sb.appendLine("失敗: ${e.javaClass.simpleName}") }
            updateOps("after bench")
            runOnUiThread { usbBench = sb.toString().trim() }
        }.start()
    }

    private fun closeUsbQuiet() {
        VolumeRegistry.removeByKey("usb:" + (usbSession?.deviceName ?: ""))
        runCatching { usbSession?.close() }
        usbSession = null
    }

    private fun closeUsb(msg: String) {
        closeUsbQuiet()
        notifyRoots()
        runOnUiThread { usbState = UiState.Failed(msg); refreshUsb() }
    }

    private fun notifyRoots() {
        contentResolver.notifyChange(android.provider.DocumentsContract.buildRootsUri("org.usbmem.app.documents"), null)
    }

    /** USB 上の NTFS パーティションを読み取り専用でマウントして SAF に公開する。 */
    private fun shareUsbPartition(offset: Long, size: Long, label: String) {
        runOnUiThread { usbState = UiState.Failed(if (usbSession?.writable == true) "NTFS をマウント中です。書き込み可のマウントは、空きクラスタ数の計算のため、大きなディスクでは数分かかります。アプリを閉じないでください…" else "NTFS をマウント中です…") }
        Thread {
            val st = try {
                val s = usbSession ?: throw IllegalStateException("not open")
                val v = s.mountNtfs(offset, size, writable = s.writable)   // 書き込み可で開いたセッションだけ rw (作成のみ)
                VolumeRegistry.addVolume(label, "usb:" + s.deviceName, v)
                notifyRoots()
                updateOps("after ntfs mount")
                val mode = if (!s.writable) "読み取り専用" else if (v.writable) "新規作成のみ書き込み可(実験的。既存のファイルは変更・削除・名前変更できません)" else "読み取り専用(書き込み不可: ビルドで無効、またはボリュームに異常/ダーティあり)"
                UiState.Failed("公開しました: $label — $mode")
            } catch (e: NativeException) {
                UiState.Failed("NTFS として開けません: ${e.message}")
            } catch (e: Exception) {
                UiState.Failed(e.javaClass.simpleName)
            }
            runOnUiThread { usbState = st }
        }.start()
    }

    override fun onStart() {
        super.onStart()
        val f = android.content.IntentFilter().apply {
            addAction(UsbStorageSession.ACTION_PERMISSION)
            addAction(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED)
        }
        if (android.os.Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, f, RECEIVER_NOT_EXPORTED) else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(usbReceiver, f)
        refreshUsb()
    }

    override fun onStop() {
        runCatching { unregisterReceiver(usbReceiver) }
        super.onStop()
    }

    override fun onDestroy() {
        if (isFinishing) closeUsbQuiet()
        super.onDestroy()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { MaterialTheme { Scaffold { pad -> Screen(Modifier.padding(pad)) } } }
    }

    @Composable
    private fun Screen(modifier: Modifier) {
        var state by remember { mutableStateOf<UiState>(UiState.Idle) }
        val scope = rememberCoroutineScope()
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri != null) scope.launch { state = withContext(Dispatchers.IO) { load(uri) } }
        }
        var allowWrite by remember { mutableStateOf(false) }   // 書き込みは毎回の起動で明示的に許可する (保存しない)
        val sharePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri != null) scope.launch { state = withContext(Dispatchers.IO) { share(uri, allowWrite) } }
        }
        Column(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("ディスクイメージ(読み取り専用)", style = MaterialTheme.typography.titleLarge)
            Button(onClick = { picker.launch(arrayOf("*/*")) }) { Text("RAW イメージを開く") }
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                androidx.compose.material3.Switch(checked = allowWrite, onCheckedChange = { allowWrite = it })
                Text("  書き込みを許可(実験的・データ破損の恐れ。バックアップ推奨)")
            }
            Button(onClick = { sharePicker.launch(arrayOf("*/*")) }) { Text(if (allowWrite) "NTFS イメージを他アプリに公開(書き込み可・実験的)" else "NTFS イメージを他アプリに公開(読み取り専用)") }
            Text(if (allowUsbWrite) "USB ストレージ(新規作成のみ書き込み可・実験的)" else "USB ストレージ(読み取り専用)", style = MaterialTheme.typography.titleMedium)
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                androidx.compose.material3.Switch(checked = allowUsbWrite, onCheckedChange = { allowUsbWrite = it }, enabled = usbSession == null)
                Text("  USB への書き込みを許可(新規作成のみ・実験的。既存のファイルは変更しません)")
            }
            if (usbSession != null) Text("※ このスイッチは USB を開く前に切り替えます。変えるには USB の抜き差し、またはアプリの再起動が必要です。", style = MaterialTheme.typography.bodySmall)
            Button(onClick = { startActivity(android.content.Intent(android.provider.Settings.ACTION_INTERNAL_STORAGE_SETTINGS)) }) { Text("OS がマウント中の USB を取り出す(ストレージ設定を開く)") }
            if (usbSession != null) {
                Button(onClick = { Thread { closeUsb("USB を閉じました(NTFS をアンマウントし、書き込みを確定しました)。取り外せます。") }.start() }) { Text("USB を安全に閉じる(アンマウントして取り外し可能にする)") }
                if (usbSession?.writable == true) Text("書き込み可で開いています。作業が終わったら必ず上のボタンで閉じてください(アプリを強制終了・USB を抜くと、NTFS がダーティのまま残り、Windows で chkdsk が走ります)。", style = MaterialTheme.typography.bodySmall)
            }
            if (usbRows.isEmpty()) Text("対応する USB マスストレージ(Bulk-Only)は見つかりません。")
            usbRows.forEach { r -> Button(onClick = { requestUsb(r.device) }) { Text(r.title) } }
            when (val u = usbState) {
                UiState.Idle -> {}
                is UiState.Failed -> Text(u.message, color = MaterialTheme.colorScheme.error)
                is UiState.Loaded -> {
                    TableView(u)
                    u.table.partitions.filter { it.fs == "ntfs" }.forEach { p ->
                        Button(onClick = { shareUsbPartition(p.startBytes, p.sizeBytes, "${u.label} #${p.index}") }) { Text(if (allowUsbWrite) "#${p.index} を他アプリに公開(NTFS・新規作成のみ書き込み可)" else "#${p.index} を他アプリに公開(NTFS・読み取り専用)") }
                    }
                    if (usbOps.isNotEmpty()) {
                        Text("送信した SCSI 命令(16進オペコードx回数): $usbOps", style = MaterialTheme.typography.bodySmall)
                        Text("読み取り系: 12 INQUIRY / 00 TUR / 03 SENSE / 1a MODE SENSE / 25,9e CAPACITY / 28,88 READ。書き込み系は 2a, 8a, 35(キャッシュ同期)。これらが無ければ書き込みなし。", style = MaterialTheme.typography.bodySmall)
                        Button(onClick = { Thread { updateOps("manual refresh") }.start() }) { Text("命令の集計を更新") }
                    }
                    Button(onClick = { benchUsb() }) { Text("読み取り速度を計測(読み取りのみ・約 1 分)") }
                    if (usbBench.isNotEmpty()) Text(usbBench, style = MaterialTheme.typography.bodySmall)
                    if (u.table.wholeDeviceVolume) Button(onClick = { shareUsbPartition(0, u.table.diskBytes, u.label) }) { Text("デバイス全体を公開(読み取り専用)") }
                }
            }
            when (val s = state) {
                UiState.Idle -> Text("イメージ(.img / .bin)を選ぶとパーティションを一覧表示します。書き込みは行いません。")
                is UiState.Failed -> Text("エラー: ${s.message}", color = MaterialTheme.colorScheme.error)
                is UiState.Loaded -> TableView(s)
            }
        }
    }

    /** NTFS イメージをマウントし、SAF プロバイダ経由でファイルマネージャ等へ公開する。 */
    private fun share(uri: Uri, writable: Boolean): UiState = try {
        val flags = android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or (if (writable) android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        try { contentResolver.takePersistableUriPermission(uri, flags) } catch (_: SecurityException) { }
        val e = VolumeRegistry.addUri(this, uri, writable)
        android.provider.DocumentsContract.buildRootsUri("org.usbmem.app.documents").let { contentResolver.notifyChange(it, null) }
        val mode = if (!writable) "読み取り専用" else if (e.volume.writable) "書き込み可(実験的)" else "読み取り専用(書き込み不可: ビルドで無効、またはボリュームに未処理の異常/ダーティあり)"
        UiState.Failed("公開しました: ${e.label} — $mode(ファイルアプリの一覧に表示されます)")
    } catch (e: NativeException) {
        UiState.Failed("NTFS として開けません: ${e.message}")
    } catch (e: Exception) {
        UiState.Failed(e.javaClass.simpleName)
    }

    private fun load(uri: Uri): UiState = try {
        // 読み取り専用 ("r") で開く。fd の所有権はネイティブ側へ渡す。
        val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return UiState.Failed("開けません")
        val fd = pfd.detachFd()
        RawImage.openReadOnly(fd).use { img -> UiState.Loaded(uri.lastPathSegment ?: "image", img.partitionTable()) }
    } catch (e: NativeException) {
        UiState.Failed(e.message ?: "native error")
    } catch (e: Exception) {
        UiState.Failed(e.javaClass.simpleName)  // パスや内容は出さない
    }

    @Composable
    private fun TableView(s: UiState.Loaded) {
        val t = s.table
        Text("${s.label}: ${t.kind.uppercase()} / ${formatBytes(t.diskBytes)} / セクタ ${t.sectorSize}B")
        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(t.partitions) { p ->
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp)) {
                        Row { Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleMedium) }
                        Text("${formatBytes(p.sizeBytes)}  開始 LBA ${p.startLba}  ${p.typeName}  [${p.fs}]" +
                            if (p.extendedContainer) "  (拡張枠)" else "")
                    }
                }
            }
            items(t.diagnostics) { d -> Text("${d.severity}: ${d.message}") }
        }
    }
}
