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

    /** 保存した転送設定の表示。null = 既定 (画面側で既定の文言にする)。 */
    private fun describeTuning(s: UsbStorageSession?): String? {
        if (s == null) return null
        val t = TransferTuning.load(this, s.vendorId, s.productId) ?: return null
        return getString(R.string.tuning_saved, t.first, t.second)
    }

    private val usbReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(c: android.content.Context, i: android.content.Intent) {
            val dev: UsbDevice? =
                if (android.os.Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            when (i.action) {
                UsbStorageSession.ACTION_PERMISSION ->
                    if (dev != null && i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) openUsb(dev)
                    else model.usbNotice = Notice(Severity.Error, getString(R.string.notice_usb_permission_denied), getString(R.string.notice_usb_permission_denied_detail))
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    // 抜去: 公開中のボリュームと接続を直ちに閉じる (抜けた後の I/O は Disconnected になるが、後始末を先に行う)
                    if (dev != null && dev.deviceName == usbSession?.deviceName) closeUsbWith(Notice(Severity.Warning, getString(R.string.notice_usb_detached)))
                    else refreshUsb()
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> refreshUsb()
            }
        }
    }

    private fun refreshUsb() {
        val list = usbManager.deviceList.values.filter { UsbMscSelector.pick(UsbMscSelector.describe(it)) != null }
        devices = list.associateBy { it.deviceName }
        model.usbDevices = list.map { UsbDeviceRow(it.deviceName, it.productName ?: getString(R.string.usb_device_default_name), String.format("%04x:%04x", it.vendorId, it.productId)) }
        // OS が USB ボリュームをマウント中か (開く前に取り出してもらう)
        usbSession?.let { model.diskIdleStopped = model.idleSpinDownMin > 0 && !model.diskStopped && it.isSpunDown() }
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
        model.busy = getString(R.string.busy_opening_usb)
        model.usbNotice = null
        Thread {
            var notice: Notice? = null
            var table: PartitionTable? = null
            try {
                closeUsbQuiet()
                val s = UsbStorageSession.open(this, usbManager, dev, writable = model.allowUsbWrite)
                usbSession = s
                applyIdleSpinDown(s)
                table = s.partitionTable()
                updateOps("after open+partition scan")
                ui { model.tuningSaved = describeTuning(s) }
            } catch (e: NativeException) {
                notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.message ?: "native error")
            } catch (e: Exception) {
                notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.javaClass.simpleName)
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
        model.busy = getString(if (sess.writable) R.string.busy_mounting_write else R.string.busy_mounting_read_only)
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
                val roText = getString(R.string.mode_read_only)
                if (!sess.writable) pub = PublishedVolume(label, roText, false)
                else if (v.writable) pub = PublishedVolume(label, getString(R.string.mode_create_only), true)
                else {
                    pub = PublishedVolume(label, roText, false)
                    notice = Notice(Severity.Warning, getString(R.string.notice_published_read_only), getString(R.string.notice_published_read_only_detail))
                }
            } catch (e: NativeException) {
                android.util.Log.w("usbmem", "ntfs mount failed: code=${e.code} ${e.message}")
                notice = if (sess.tuningIndex >= 0) {   // 保存した転送設定が原因かもしれない → 既定に戻す (次の最適化で、その設定以上は試さない)
                    TransferTuning.markFailed(this, sess.vendorId, sess.productId, sess.tuningIndex)
                    TransferTuning.reset(this, sess.vendorId, sess.productId)
                    Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed_with_reason, e.message ?: ""), getString(R.string.notice_ntfs_open_failed_tuning_reset))
                } else Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.message)
            } catch (e: Exception) {
                notice = Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.javaClass.simpleName)
            }
            ui { model.busy = null; model.published = pub; model.usbNotice = notice }
        }.start()
    }

    /**
     * ディスクを止める: 公開中のボリュームを閉じて (アンマウント + 書き込みの確定)、SCSI START STOP UNIT で回転を止める。
     * セッション (USB の確保) は保つ: 閉じると OS の usb-storage が戻って FAT をマウントし、ディスクがまた動くため。止めた後は、USB を抜く。
     */
    private fun settingsPrefs() = getSharedPreferences("usbmem_settings", MODE_PRIVATE)

    /** アイドル自動スピンダウンの設定 (分、0 = オフ)。保存して、開いているセッションにも反映する。 */
    override fun setIdleSpinDown(minutes: Int) {
        model.idleSpinDownMin = minutes
        runCatching { settingsPrefs().edit().putInt("idle_spindown_min", minutes).apply() }
        applyIdleSpinDown(usbSession)
    }

    private fun applyIdleSpinDown(s: UsbStorageSession?) {
        s?.setIdleSpinDown(model.idleSpinDownMin) { stopped -> ui { if (stopped && !model.diskIdleStopped) model.usbNotice = null; model.diskIdleStopped = stopped } }
        if (model.idleSpinDownMin == 0) model.diskIdleStopped = false
    }

    override fun stopDisk() {
        val s = usbSession ?: return
        model.busy = getString(R.string.busy_stopping_disk)
        model.usbNotice = null
        Thread {
            var notice: Notice
            var stopped = false
            VolumeRegistry.removeByKey("usb:" + s.deviceName)   // アンマウント + 書き込みの確定
            notifyRoots()
            try {
                s.startStop(false)
                stopped = true
                notice = Notice(Severity.Success, getString(R.string.notice_disk_stopped), getString(R.string.notice_disk_stopped_detail))
            } catch (e: NativeException) {
                android.util.Log.w("usbmem", "stop disk failed: code=${e.code} ${e.message}")
                notice = if (e.code == 9) Notice(Severity.Warning, getString(R.string.notice_disk_stop_unsupported), getString(R.string.notice_disk_stop_unsupported_detail))
                else Notice(Severity.Error, getString(R.string.notice_disk_stop_failed), e.message)
            }
            updateOps("after stop disk")
            ui { model.busy = null; model.published = null; model.diskStopped = stopped; model.usbNotice = notice }
        }.start()
    }

    override fun resumeDisk() {
        val s = usbSession ?: return
        model.busy = getString(R.string.busy_resuming_disk)
        Thread {
            var notice: Notice
            var ok = false
            try { s.startStop(true); ok = true; notice = Notice(Severity.Success, getString(R.string.notice_disk_resumed)) }
            catch (e: NativeException) { notice = Notice(Severity.Error, getString(R.string.notice_disk_stop_failed), e.message) }
            updateOps("after resume disk")
            ui { model.busy = null; if (ok) model.diskStopped = false; model.usbNotice = notice }
        }.start()
    }

    override fun closeUsb() {
        model.busy = getString(R.string.busy_closing_usb)
        Thread { closeUsbWith(Notice(Severity.Success, getString(R.string.notice_usb_closed), getString(R.string.notice_usb_closed_detail))) }.start()
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
            model.busy = null; model.usbOpenKey = null; model.usbWritable = false; model.table = null; model.published = null; model.diskStopped = false; model.diskIdleStopped = false
            model.usbNotice = notice; model.ops = ""; model.tuneLog = ""; model.tuningSaved = null
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
        model.busy = getString(R.string.busy_benchmark)
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
                        getString(R.string.diag_bench_line, chunk, mt, total / 1048576.0 / (us / 1e6))
                    } catch (e: NativeException) { getString(R.string.diag_bench_line_failed, chunk, mt, e.message ?: "") }
                    android.util.Log.i("usbmem", "bench $line"); sb.appendLine(line)
                }
                val saved = TransferTuning.load(this, s.vendorId, s.productId)
                s.tune(saved?.first ?: UsbStorageSession.READ_CHUNK_KIB, saved?.second ?: UsbStorageSession.READ_MAX_TRANSFER_KIB)   // 保存済み (無ければ既定) へ戻す
            } catch (e: Exception) { sb.appendLine(getString(R.string.diag_failed_simple, e.javaClass.simpleName)) }
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
        model.busy = getString(R.string.busy_optimize)
        Thread {
            val sb = StringBuilder()
            val ladder = TransferTuning.LADDER
            val vid = s.vendorId; val pid = s.productId
            var best = 0; var bestMbps = 0.0
            try {
                val limit = TransferTuning.failedFrom(this, vid, pid)
                if (limit < ladder.size) {   // 前回、その設定の試行中に落ちた/失敗した → 失敗として確定
                    TransferTuning.markFailed(this, vid, pid, limit)
                    sb.appendLine(getString(R.string.diag_opt_skip_failed, ladder[limit].first, ladder[limit].second))
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
                            sb.appendLine(getString(R.string.diag_opt_mismatch, chunk, mt))
                            break
                        }
                    }
                    val off = 1024 * mb + 64 * mb + i * 96 * mb   // 区間はずらして、デバイス側のキャッシュ効果を避ける
                    val us = s.benchReadHash(off, 64 * mb, 1024, h)
                    val mbps = 64.0 / (us / 1e6)
                    sb.appendLine(getString(R.string.diag_opt_line_ok, chunk, mt, mbps))
                    android.util.Log.i("usbmem", "optimize chunk=${chunk}K cmd=${mt}K: %.1f MB/s".format(mbps))
                    if (mbps > bestMbps * 1.03) { best = i; bestMbps = mbps }
                    TransferTuning.markOk(this, vid, pid, best)   // 途中経過も保存 (試行中の印はここで消える)
                }
                s.tune(ladder[best].first, ladder[best].second)
                sb.appendLine(getString(R.string.diag_opt_saved, ladder[best].first, ladder[best].second, bestMbps))
            } catch (e: Exception) {
                val code = (e as? NativeException)?.code
                android.util.Log.w("usbmem", "optimize failed: ${e.message}")
                // 失敗した試行は「試行中」の印が残っている → 次回の最適化で失敗として扱われる。ここでも確定させておく
                val trying = TransferTuning.failedFrom(this, vid, pid)
                if (trying < ladder.size) TransferTuning.markFailed(this, vid, pid, trying)
                runCatching { s.tune(ladder[best].first, ladder[best].second) }
                sb.appendLine(getString(R.string.diag_opt_failed, "${e.message}${if (code != null) ", code=$code" else ""}"))
                sb.appendLine(getString(R.string.diag_opt_failed_hint))
                sb.appendLine(getString(R.string.diag_opt_best_so_far, ladder[best].first, ladder[best].second))
            }
            updateOps("after optimize")
            ui { model.busy = null; model.tuneLog = sb.toString().trim(); model.tuningSaved = describeTuning(s) }
        }.start()
    }

    // ---- ImageActions (IO スレッドから呼ばれる)
    override fun loadImage(uri: Uri): ImageResult = try {
        // 読み取り専用 ("r") で開く。fd の所有権はネイティブ側へ渡す。
        val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_image_open_failed)))
        val fd = pfd.detachFd()
        RawImage.openReadOnly(fd).use { img -> ImageResult(label = uri.lastPathSegment ?: "image", table = img.partitionTable()) }
    } catch (e: NativeException) {
        ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_image_open_failed), e.message ?: "native error"))
    } catch (e: Exception) {
        ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_image_open_failed), e.javaClass.simpleName))  // パスや内容は出さない
    }

    /** NTFS イメージをマウントし、SAF プロバイダ経由でファイルマネージャ等へ公開する。 */
    override fun shareImage(uri: Uri, writable: Boolean): ImageResult = try {
        val flags = android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or (if (writable) android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        try { contentResolver.takePersistableUriPermission(uri, flags) } catch (_: SecurityException) { }
        val e = VolumeRegistry.addUri(this, uri, writable)
        notifyRoots()
        if (!writable) ImageResult(notice = Notice(Severity.Success, getString(R.string.notice_image_shared_read_only, e.label), getString(R.string.notice_image_shared_read_only_detail)))
        else if (e.volume.writable) ImageResult(notice = Notice(Severity.Warning, getString(R.string.notice_image_shared_write, e.label), getString(R.string.notice_image_shared_write_detail)))
        else ImageResult(notice = Notice(Severity.Warning, getString(R.string.notice_image_shared_fallback, e.label), getString(R.string.notice_image_shared_fallback_detail)))
    } catch (e: NativeException) {
        ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.message))
    } catch (e: Exception) {
        ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.javaClass.simpleName))
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
        model.idleSpinDownMin = runCatching { settingsPrefs().getInt("idle_spindown_min", 0) }.getOrDefault(0)
        setContent { AppTheme { AppScaffold(model, this, this, this) } }
    }
}
