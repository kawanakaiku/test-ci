package org.usbmem.app

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import org.usbmem.app.ui.AppModel
import org.usbmem.app.ui.AppScaffold
import org.usbmem.app.ui.AppTheme
import org.usbmem.app.ui.DiagnosticsActions
import org.usbmem.app.ui.ImageActions
import org.usbmem.app.ui.ImageResult
import org.usbmem.app.ui.Notice
import org.usbmem.app.ui.PublishedVolume
import org.usbmem.app.ui.Severity
import org.usbmem.app.ui.UsbActions
import org.usbmem.app.ui.UsbDeviceRow

/**
 * 画面 (ui/) と、USB・NTFS の処理をつなぐ。画面は AppModel の値を読むだけで、操作はこのクラスの *Actions 実装が受ける。
 * USB のセッションはこの Activity が持つ (Activity が終わるとき、公開中のボリュームごと閉じる)。
 */
class MainActivity : ComponentActivity(), UsbActions, ImageActions, DiagnosticsActions {
    private val usbManager by lazy { getSystemService(USB_SERVICE) as UsbManager }
    private var usbSession: UsbStorageSession? = null
    private var devices: Map<String, UsbDevice> = emptyMap()
    private val model = AppModel()

    private fun ui(block: () -> Unit) = runOnUiThread(block)

    private fun updateOps(tag: String) {
        val o = usbSession?.opStats() ?: return
        android.util.Log.i("usbmem", "SCSI ops [$tag]: $o")
        ui { model.ops = o }
    }

    private fun describeTuning(s: UsbStorageSession?): String {
        if (s == null) return "既定 (USB 16K / コマンド 64K)"
        val t = TransferTuning.load(this, s.vendorId, s.productId) ?: return "既定 (USB 16K / コマンド 64K)"
        return "USB ${t.first}K / コマンド ${t.second}K (この端末で保存した設定)"
    }

    private val usbReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(c: android.content.Context, i: android.content.Intent) {
            val dev: UsbDevice? =
                if (android.os.Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            when (i.action) {
                UsbStorageSession.ACTION_PERMISSION ->
                    if (dev != null && i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) openUsb(dev)
                    else model.usbNotice = Notice(Severity.Error, "USB の許可が得られませんでした", "もう一度「開く」を押して、許可ダイアログで許可してください。")
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    // 抜去: 公開中のボリュームと接続を直ちに閉じる (抜けた後の I/O は Disconnected になるが、後始末を先に行う)
                    if (dev != null && dev.deviceName == usbSession?.deviceName) closeUsbWith(Notice(Severity.Warning, "USB デバイスが取り外されました"))
                    else refreshUsb()
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> refreshUsb()
            }
        }
    }

    private fun refreshUsb() {
        val list = usbManager.deviceList.values.filter { UsbMscSelector.pick(UsbMscSelector.describe(it)) != null }
        devices = list.associateBy { it.deviceName }
        model.usbDevices = list.map { UsbDeviceRow(it.deviceName, it.productName ?: "USB ストレージ", String.format("%04x:%04x", it.vendorId, it.productId)) }
        // OS が USB ボリュームをマウント中か (開く前に取り出してもらう)
        model.osMountedUsb = runCatching { UsbStorageSession.osMountedUsbVolumes(java.io.File("/proc/mounts").readText()).size }.getOrDefault(0)
    }

    // ---- UsbActions
    override fun openStorageSettings() {
        startActivity(android.content.Intent(android.provider.Settings.ACTION_INTERNAL_STORAGE_SETTINGS))
    }

    override fun setAllowWrite(v: Boolean) { if (usbSession == null) model.allowUsbWrite = v }

    override fun openUsb(key: String) {
        val dev = devices[key] ?: return
        if (usbManager.hasPermission(dev)) openUsb(dev) else usbManager.requestPermission(dev, UsbStorageSession.permissionIntent(this))
    }

    private fun openUsb(dev: UsbDevice) {
        model.busy = "USB を開いています…"
        model.usbNotice = null
        Thread {
            var notice: Notice? = null
            var table: PartitionTable? = null
            try {
                closeUsbQuiet()
                val s = UsbStorageSession.open(this, usbManager, dev, writable = model.allowUsbWrite)
                usbSession = s
                table = s.partitionTable()
                updateOps("after open+partition scan")
                ui { model.tuningSaved = describeTuning(s) }
            } catch (e: NativeException) {
                notice = Notice(Severity.Error, "USB を開けません", e.message ?: "native error")
            } catch (e: Exception) {
                notice = Notice(Severity.Error, "USB を開けません", e.javaClass.simpleName)
            }
            val s = usbSession
            ui {
                model.busy = null
                model.published = null
                model.usbNotice = notice
                if (table != null && s != null) {
                    model.usbOpenKey = dev.deviceName
                    model.usbWritable = s.writable
                    model.usbLabel = dev.productName ?: "USB"
                    model.table = table
                } else { model.usbOpenKey = null; model.table = null }
                refreshUsb()
            }
        }.start()
    }

    /** USB 上の NTFS パーティションをマウントして SAF に公開する (開いたセッションが書き込み可なら、新規作成のみ書き込み可)。 */
    override fun publishPartition(startBytes: Long, sizeBytes: Long, label: String) {
        val sess = usbSession ?: return
        model.busy = if (sess.writable) "NTFS をマウント中です。書き込み可のマウントは、空きクラスタ数の計算のため、大きなディスクでは数十秒かかります。アプリを閉じないでください…" else "NTFS をマウント中です…"
        model.usbNotice = null
        Thread {
            var notice: Notice? = null
            var pub: PublishedVolume? = null
            try {
                val v = sess.mountNtfs(startBytes, sizeBytes, writable = sess.writable)
                VolumeRegistry.addVolume(label, "usb:" + sess.deviceName, v)
                notifyRoots()
                updateOps("after ntfs mount")
                android.util.Log.i("usbmem", "ntfs mount: sessionWritable=${sess.writable} volumeWritable=${v.writable} createOnly=${v.createOnly}")
                if (!sess.writable) pub = PublishedVolume(label, "読み取り専用", false)
                else if (v.writable) pub = PublishedVolume(label, "新規作成のみ書き込み可", true)
                else {
                    pub = PublishedVolume(label, "読み取り専用", false)
                    notice = Notice(Severity.Warning, "読み取り専用で公開しました", "書き込みは使えません (ビルドで無効、またはボリュームに異常/ダーティがあります)。")
                }
            } catch (e: NativeException) {
                android.util.Log.w("usbmem", "ntfs mount failed: code=${e.code} ${e.message}")
                notice = if (sess.tuningIndex >= 0) {   // 保存した転送設定が原因かもしれない → 既定に戻す (次の最適化で、その設定以上は試さない)
                    TransferTuning.markFailed(this, sess.vendorId, sess.productId, sess.tuningIndex)
                    TransferTuning.reset(this, sess.vendorId, sess.productId)
                    Notice(Severity.Error, "NTFS として開けません: ${e.message}", "保存した転送設定を使っていたため、既定に戻しました。USB を閉じて、開き直してください。")
                } else Notice(Severity.Error, "NTFS として開けません", e.message)
            } catch (e: Exception) {
                notice = Notice(Severity.Error, "NTFS として開けません", e.javaClass.simpleName)
            }
            ui { model.busy = null; model.published = pub; model.usbNotice = notice }
        }.start()
    }

    override fun closeUsb() {
        model.busy = "USB を閉じています (書き込みの確定とアンマウント)…"
        Thread { closeUsbWith(Notice(Severity.Success, "USB を閉じました", "NTFS をアンマウントし、書き込みを確定しました。取り外せます。")) }.start()
    }

    private fun closeUsbQuiet() {
        VolumeRegistry.removeByKey("usb:" + (usbSession?.deviceName ?: ""))
        runCatching { usbSession?.close() }
        usbSession = null
    }

    private fun closeUsbWith(notice: Notice) {
        closeUsbQuiet()
        notifyRoots()
        ui {
            model.busy = null; model.usbOpenKey = null; model.usbWritable = false; model.table = null; model.published = null
            model.usbNotice = notice; model.ops = ""; model.tuneLog = ""; model.tuningSaved = describeTuning(null)
            refreshUsb()
        }
    }

    private fun notifyRoots() {
        contentResolver.notifyChange(android.provider.DocumentsContract.buildRootsUri("org.usbmem.app.documents"), null)
    }

    // ---- DiagnosticsActions
    override fun refreshOps() { Thread { updateOps("manual refresh") }.start() }

    /** 読み取り専用の速度計測。転送サイズの組を順に試し、MB/s を表示・ログへ出す。デバイスの先頭付近を読むだけで、何も書かない。 */
    override fun benchmark() {
        val s = usbSession ?: return
        model.busy = "読み取り速度を計測しています…"
        Thread {
            val sb = StringBuilder()
            try {
                val total = 128L shl 20
                // (USB 1 回の転送要求 KiB, 1 SCSI コマンド KiB)。区間はずらして、デバイス側のキャッシュ効果を避ける
                TransferTuning.LADDER.forEachIndexed { i, (chunk, mt) ->
                    val off = (1L shl 30) + i * (256L shl 20)
                    val line = try {
                        s.tune(chunk, mt)
                        val us = s.benchRead(off, total, 1024)
                        "chunk=${chunk}K cmd=${mt}K: %.1f MB/s".format(total / 1048576.0 / (us / 1e6))
                    } catch (e: NativeException) { "chunk=${chunk}K cmd=${mt}K: 失敗 ${e.message}" }
                    android.util.Log.i("usbmem", "bench $line"); sb.appendLine(line)
                }
                val saved = TransferTuning.load(this, s.vendorId, s.productId)
                s.tune(saved?.first ?: UsbStorageSession.READ_CHUNK_KIB, saved?.second ?: UsbStorageSession.READ_MAX_TRANSFER_KIB)   // 保存済み (無ければ既定) へ戻す
            } catch (e: Exception) { sb.appendLine("失敗: ${e.javaClass.simpleName}") }
            updateOps("after bench")
            ui { model.busy = null; model.tuneLog = sb.toString().trim() }
        }.start()
    }

    /**
     * 読み取り転送サイズの最適化 (読み取りのみ。何も書かない)。小さい設定から順に、同じ区間を読んでハッシュが一致するか + 速度を確かめ、**通った中で最速**を、この端末・この USB 機器用に保存する。
     * 保存した設定は次回から USB を開くたびに使われる (書き込みの転送サイズは変えない)。失敗した設定は記録して二度と試さない。
     * 失敗すると USB が切れて OS が FAT を再マウントすることがある (J9260)。その場合は USB を閉じて、OS の FAT を取り出し、開き直す。
     */
    override fun optimize() {
        val s = usbSession ?: return
        model.busy = "読み取りを最適化しています…"
        Thread {
            val sb = StringBuilder()
            val ladder = TransferTuning.LADDER
            val vid = s.vendorId; val pid = s.productId
            var best = 0; var bestMbps = 0.0
            try {
                val limit = TransferTuning.failedFrom(this, vid, pid)
                if (limit < ladder.size) {   // 前回、その設定の試行中に落ちた/失敗した → 失敗として確定
                    TransferTuning.markFailed(this, vid, pid, limit)
                    sb.appendLine("前回失敗した設定 (chunk=${ladder[limit].first}K cmd=${ladder[limit].second}K 以上) は試しません")
                }
                val mb = 1L shl 20
                val refOff = 1024 * mb; val refLen = 4 * mb
                val h = LongArray(1)
                s.tune(ladder[0].first, ladder[0].second)
                s.benchReadHash(refOff, refLen, 1024, h); val refHash = h[0]
                for (i in 0 until minOf(limit, ladder.size)) {
                    val (chunk, mt) = ladder[i]
                    if (i > 0) {
                        TransferTuning.markTrying(this, vid, pid, i)   // 落ちたら、次回この設定以上を試さない
                        s.tune(chunk, mt)
                        s.benchReadHash(refOff, refLen, 1024, h)
                        if (h[0] != refHash) {   // 転送サイズを変えたら内容が変わった = 化けている。使わない
                            TransferTuning.markFailed(this, vid, pid, i)
                            sb.appendLine("chunk=${chunk}K cmd=${mt}K: 読めたデータが一致しません。この設定は使いません")
                            break
                        }
                    }
                    val off = 1024 * mb + 64 * mb + i * 96 * mb   // 区間はずらして、デバイス側のキャッシュ効果を避ける
                    val us = s.benchReadHash(off, 64 * mb, 1024, h)
                    val mbps = 64.0 / (us / 1e6)
                    sb.appendLine("chunk=${chunk}K cmd=${mt}K: %.1f MB/s (データ一致)".format(mbps))
                    android.util.Log.i("usbmem", "optimize chunk=${chunk}K cmd=${mt}K: %.1f MB/s".format(mbps))
                    if (mbps > bestMbps * 1.03) { best = i; bestMbps = mbps }
                    TransferTuning.markOk(this, vid, pid, best)   // 途中経過も保存 (試行中の印はここで消える)
                }
                s.tune(ladder[best].first, ladder[best].second)
                sb.appendLine("→ 保存: chunk=${ladder[best].first}K cmd=${ladder[best].second}K (%.1f MB/s)。次回から USB を開くときに使います".format(bestMbps))
            } catch (e: Exception) {
                val code = (e as? NativeException)?.code
                android.util.Log.w("usbmem", "optimize failed: ${e.message}")
                // 失敗した試行は「試行中」の印が残っている → 次回の最適化で失敗として扱われる。ここでも確定させておく
                val trying = TransferTuning.failedFrom(this, vid, pid)
                if (trying < ladder.size) TransferTuning.markFailed(this, vid, pid, trying)
                runCatching { s.tune(ladder[best].first, ladder[best].second) }
                sb.appendLine("失敗しました (${e.message}${if (code != null) ", code=$code" else ""})。この設定は使いません。")
                sb.appendLine("USB が切れた場合は、USB を閉じる → OS が FAT をマウントしていたら取り出し → もう一度 USB を開いてください。")
                sb.appendLine("ここまでに通った最速: chunk=${ladder[best].first}K cmd=${ladder[best].second}K (保存済み)")
            }
            updateOps("after optimize")
            ui { model.busy = null; model.tuneLog = sb.toString().trim(); model.tuningSaved = describeTuning(s) }
        }.start()
    }

    // ---- ImageActions (IO スレッドから呼ばれる)
    override fun loadImage(uri: Uri): ImageResult = try {
        // 読み取り専用 ("r") で開く。fd の所有権はネイティブ側へ渡す。
        val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return ImageResult(notice = Notice(Severity.Error, "開けません"))
        val fd = pfd.detachFd()
        RawImage.openReadOnly(fd).use { img -> ImageResult(label = uri.lastPathSegment ?: "image", table = img.partitionTable()) }
    } catch (e: NativeException) {
        ImageResult(notice = Notice(Severity.Error, "開けません", e.message ?: "native error"))
    } catch (e: Exception) {
        ImageResult(notice = Notice(Severity.Error, "開けません", e.javaClass.simpleName))  // パスや内容は出さない
    }

    /** NTFS イメージをマウントし、SAF プロバイダ経由でファイルマネージャ等へ公開する。 */
    override fun shareImage(uri: Uri, writable: Boolean): ImageResult = try {
        val flags = android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or (if (writable) android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        try { contentResolver.takePersistableUriPermission(uri, flags) } catch (_: SecurityException) { }
        val e = VolumeRegistry.addUri(this, uri, writable)
        notifyRoots()
        if (!writable) ImageResult(notice = Notice(Severity.Success, "公開しました: ${e.label}", "読み取り専用。ファイルアプリの一覧に表示されます。"))
        else if (e.volume.writable) ImageResult(notice = Notice(Severity.Warning, "書き込み可 (実験的) で公開しました: ${e.label}", "ファイルアプリの一覧に表示されます。バックアップを取ってから使ってください。"))
        else ImageResult(notice = Notice(Severity.Warning, "読み取り専用で公開しました: ${e.label}", "書き込みは使えません (ビルドで無効、またはボリュームに未処理の異常/ダーティがあります)。"))
    } catch (e: NativeException) {
        ImageResult(notice = Notice(Severity.Error, "NTFS として開けません", e.message))
    } catch (e: Exception) {
        ImageResult(notice = Notice(Severity.Error, "NTFS として開けません", e.javaClass.simpleName))
    }

    // ---- ライフサイクル
    override fun onStart() {
        super.onStart()
        val f = android.content.IntentFilter().apply {
            addAction(UsbStorageSession.ACTION_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        }
        if (android.os.Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, f, RECEIVER_NOT_EXPORTED) else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(usbReceiver, f)
        refreshUsb()
    }

    override fun onResume() {
        super.onResume()
        refreshUsb()   // ストレージ設定から戻ったとき、OS のマウント状態が変わっている
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
        enableEdgeToEdge()
        setContent { AppTheme { AppScaffold(model, this, this, this) } }
    }
}
