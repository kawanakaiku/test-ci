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
import org.usbmem.app.ui.NestedInspectResult
import org.usbmem.app.ui.NestedMountResult
import org.usbmem.app.ui.NestedVolumeRow
import org.usbmem.app.ui.NoloadPrompt
import org.usbmem.app.ui.PublishedVolume
import org.usbmem.app.ui.Severity
import org.usbmem.app.ui.UsbActions
import org.usbmem.app.ui.UsbDeviceState

/**
 * 画面 (ui/) と、USB・NTFS の処理をつなぐ。画面は AppModel の値を読むだけで、操作はこのクラスの *Actions 実装が受ける。
 *
 * USB のセッションは**プロセスが持つ** ([UsbSessions] / [UsbSessionManager]。デバイス名をキーに複数本)。この Activity は表示と操作の入口で、
 * 作り直されてもセッションは失われない。Activity が終わるとき (isFinishing) は、全部のデバイスを公開中のボリュームごと閉じる。
 * 操作はすべてデバイス名 (key) で対象を指定し、他のデバイスには影響しない。docs/multi-usb.md
 */
class MainActivity : ComponentActivity(), UsbActions, ImageActions, DiagnosticsActions {
    private val usbManager by lazy { getSystemService(USB_SERVICE) as UsbManager }
    private val sessions by lazy { UsbSessions.manager(this) }
    private val model = AppModel.shared
    /** 権限を得た機器のシリアル (識別子の元)。メインスレッドだけで読み書きする */
    private val serials = HashMap<String, String>()
    /** 複数 LUN と分かったデバイスの LUN 一覧 (デバイス名 → 調査結果)。メインスレッドだけで読み書きする。ここにあるデバイスは LUN ごとのカードで表示する */
    private val lunScans = HashMap<String, UsbLunScan>()
    /** 権限の確認を待っている間に覚えておく、開く LUN (null = 自動)。確認が済んだら続きを開く */
    private val permLun = HashMap<String, Int?>()

    private fun ui(block: () -> Unit) = runOnUiThread(block)

    private fun state(key: String): UsbDeviceState? = model.device(key)

    private fun updateOps(key: String, tag: String) {
        val o = sessions.session(key)?.opStats() ?: return
        android.util.Log.i("usbmem", "SCSI ops [$key $tag]: $o")
        ui { state(key)?.ops = o }
    }

    /** 保存した転送設定の表示。null = 既定 (画面側で既定の文言にする)。 */
    private fun describeTuning(s: UsbSession?): String? {
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
                    if (dev != null && i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) openUsb(dev, permLun.remove(dev.deviceName))
                    else {
                        val d = dev?.let { state(it.deviceName) }
                        val n = Notice(Severity.Error, if (d != null) getString(R.string.notice_usb_permission_denied_named, d.title) else getString(R.string.notice_usb_permission_denied), getString(R.string.notice_usb_permission_denied_detail))
                        if (d != null) d.notice = n else model.usbNotice = n
                    }
                // 抜去の後始末 (そのデバイスのセッションだけを閉じる) は UsbApp の受信側と、リスナ (下) が行う。ここは一覧を更新するだけ
                UsbManager.ACTION_USB_DEVICE_DETACHED, UsbManager.ACTION_USB_DEVICE_ATTACHED -> refreshUsb()
            }
        }
    }

    /** セッションの変化を画面へ。呼び出しはどのスレッドからでも (UsbApp の抜去処理・アイドル監視スレッドなど)。 */
    private val sessionListener = object : UsbSessionListener {
        override fun onClosed(deviceName: String, reason: CloseReason) = ui {
            val d = state(deviceName)
            if (d != null) {
                val title = d.title
                d.resetAfterClose()
                if (reason == CloseReason.Detached) model.usbNotice = Notice(Severity.Warning, getString(R.string.notice_usb_detached_named, title), getString(R.string.notice_usb_detached_named_detail))
            }
            refreshUsb()
        }

        override fun onIdleChanged(deviceName: String, stopped: Boolean) = ui { state(deviceName)?.diskIdleStopped = stopped }
    }

    private fun readMounts(): String? = try { java.io.File("/proc/mounts").readText() } catch (_: Exception) { null }

    private fun infoOf(dev: UsbDevice): UsbDeviceInfo {
        if (usbManager.hasPermission(dev) && !serials.containsKey(dev.deviceName)) runCatching { dev.serialNumber }.getOrNull()?.let { serials[dev.deviceName] = it }
        return UsbDeviceInfo(dev.deviceName, dev.productName ?: getString(R.string.usb_device_default_name), dev.vendorId, dev.productId, serials[dev.deviceName])
    }

    // vidPid は設定の識別子 (UsbDeviceState.settingsId): LUN 0 は "0781:5581" (従来のキーと同じ)、LUN 1 以上は "0781:5581/l1"
    private fun idleKey(vidPid: String) = "idle_spindown_min:$vidPid"
    /** 回転ディスク以外でスピンダウンを使うと確認した種別 (MediaKind の名前)。機種 (VID:PID) ごと。種別が変わったら無効に倒す (UsbSessionManager が照合) */
    private fun spinAckKey(vidPid: String) = "spindown_ack:$vidPid"
    private fun loadSpinAck(vidPid: String): MediaKind? = runCatching { settingsPrefs().getString(spinAckKey(vidPid), null) }.getOrNull()?.let { n -> MediaKind.entries.firstOrNull { it.name == n } }?.takeIf { it != MediaKind.Rotational }

    /**
     * 画面の一覧を、接続中の USB とセッションの実際の状態に合わせる (メインスレッド)。抜かれたのにまだ開いているセッションがあれば閉じる。
     * 複数 LUN と分かったデバイス ([lunScans]) は LUN ごとに 1 枚 (スロット名がキー)、それ以外は 1 本 1 枚。
     */
    private fun refreshUsb() {
        val list = usbManager.deviceList.values.filter { UsbMscSelector.pick(UsbMscSelector.describe(it)) != null }.sortedBy { it.deviceName }
        val infos = list.map { infoOf(it) }
        val ids = UsbDeviceNaming.shortIds(infos)
        val old = model.usbDevices.associateBy { it.key }
        val mounts = readMounts()
        val next = ArrayList<UsbDeviceState>()
        for ((dev, info) in list.zip(infos)) {
            val scan = lunScans[dev.deviceName]?.takeIf { it.multi }
            val vp = String.format("%04x:%04x", dev.vendorId, dev.productId)
            val shortId = ids.getValue(dev.deviceName)
            // OS がこのデバイスのボリュームをマウント中か (開く前に取り出してもらう)。他の USB のマウントは数えない。デバイス単位なので、どの LUN のカードにも同じ値
            val osBlocked = if (mounts == null) 0 else UsbStorageSession.osMountedBlocking(mounts, dev.deviceName) { UsbStorageSession.sysfsOwnerOfVoldNode(it) }.size
            for (lun in 0 until (scan?.lunCount ?: 1)) {
                val key = UsbSlotKey.of(dev.deviceName, lun)
                val sid = if (lun == 0) vp else "$vp/l$lun"
                val st = old[key] ?: UsbDeviceState(key).also {
                    // アイドル停止の設定は機種 (VID:PID) ごとに覚えておく (無ければ従来の全体設定を初期値に)。複数 LUN は LUN ごと
                    it.idleSpinDownMin = runCatching { settingsPrefs().getInt(idleKey(sid), settingsPrefs().getInt("idle_spindown_min", 0)) }.getOrDefault(0)
                    sessions.setSpinDownConfirmed(key, loadSpinAck(sid))   // 先に確認済みの種別を渡す (開いたときに照合される)
                    sessions.setIdleSpinDownMinutes(key, it.idleSpinDownMin)
                }
                val li = scan?.luns?.firstOrNull { it.lun == lun }?.let { n -> if (n.state == UsbLunState.Skipped && st.lunInfo != null) st.lunInfo!! else n }   // 調べなかった (開いている) LUN は前の情報のまま
                st.lunInfo = li
                st.lunCount = scan?.lunCount ?: 1
                st.settingsId = sid
                st.productName = info.productName
                st.shortId = shortId
                st.title = if (scan != null) UsbDeviceNaming.lunTitle(info.productName, shortId, lun, li?.sizeBytes ?: 0) else UsbDeviceNaming.displayName(info.productName, shortId)
                st.vidPid = vp
                st.writeMode = sessions.writeMode(key)
                if (st.open && !sessions.isBusy(key)) st.resetAfterClose()   // 画面が知らない間に閉じていた
                st.osBlocked = osBlocked
                next.add(st)
            }
        }
        model.usbDevices = next
        // 抜かれたのに残っているセッション (画面が止まっていて抜去の通知を受けなかった場合など) を、そのデバイスだけ閉じる。LUN 一覧のために保っていた接続も手放す
        val present = list.map { it.deviceName }.toSet()
        for (k in old.keys.map { UsbSlotKey.deviceOf(it) }.toSet() - present) {
            serials.remove(k)
            val scanned = lunScans.remove(k) != null
            permLun.remove(k)
            if (scanned || sessions.isDeviceBusy(k)) Thread({ sessions.onDetached(k) }, "usbmem-detach").start()
        }
    }

    // ---- UsbActions
    override fun openStorageSettings() {
        startActivity(android.content.Intent(android.provider.Settings.ACTION_INTERNAL_STORAGE_SETTINGS))
    }

    /** そのデバイスの書き込みモードを変える。開く前 (開く最中・閉じる最中でない) だけ。ビルドで書き込みが無効なら読み取り専用以外は選べない。 */
    override fun setWriteMode(key: String, mode: WriteMode) {
        if (sessions.setWriteMode(key, mode)) state(key)?.writeMode = mode
    }

    override fun openUsb(key: String) {
        val dev = usbManager.deviceList[UsbSlotKey.deviceOf(key)] ?: return
        val d = state(key) ?: return
        if (d.open || d.busy != null) return
        // 複数 LUN と分かっているデバイスは、押されたカードの LUN を開く。まだ分からないデバイスは null (LUN 数を調べてから決める)
        val lun = if (d.lunInfo != null) d.lun else null
        if (d.lunInfo?.openable == false) return
        if (usbManager.hasPermission(dev)) openUsb(dev, lun) else { permLun[dev.deviceName] = lun; usbManager.requestPermission(dev, UsbStorageSession.permissionIntent(this)) }
    }

    /**
     * [lun] = null: LUN の数を調べる (GET MAX LUN)。LUN 1 つなら LUN 0 を開く。複数なら LUN ごとのカードにして、メディアのある LUN がちょうど 1 つならそれを開く
     * (複数あれば、使う LUN のカードを選んでもらう)。[lun] != null: その LUN を開く。
     */
    private fun openUsb(dev: UsbDevice, lun: Int?) {
        val origKey = UsbSlotKey.of(dev.deviceName, lun ?: 0)
        val d = state(origKey) ?: return
        if (d.open || d.busy != null) return
        d.busy = getString(R.string.busy_opening_usb)
        d.notice = null
        val info = infoOf(dev)
        if (lun == null) scanThenOpen(dev, info, origKey) else openLun(dev, info, lun)
    }

    /** LUN 一覧を取って、結果をメインスレッドで反映する (rescan も同じ)。[thenOpen] = 取った後に、開く LUN を決めて続けて開く。 */
    private fun scanThenOpen(dev: UsbDevice, info: UsbDeviceInfo, origKey: String, thenOpen: Boolean = true) {
        Thread {
            var scan: UsbLunScan? = null
            var notice: Notice? = null
            try { scan = sessions.scanLuns(info) }
            catch (e: NativeException) { notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.message ?: "native error") }
            catch (e: SecurityException) { notice = Notice(Severity.Error, getString(R.string.notice_usb_permission_denied)) }
            catch (e: Exception) { notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.javaClass.simpleName) }
            val sc = scan; val n = notice
            ui {
                val st = state(origKey)
                st?.busy = null
                if (sc == null) { st?.notice = n; refreshUsb(); return@ui }
                if (!usbManager.deviceList.containsKey(dev.deviceName)) { refreshUsb(); return@ui }   // 調べている間に抜かれた
                if (sc.multi) lunScans[dev.deviceName] = sc else lunScans.remove(dev.deviceName)
                refreshUsb()
                when {
                    !thenOpen -> {}
                    !sc.multi -> openUsb(dev, 0)
                    sc.ready.size == 1 -> openUsb(dev, sc.ready[0].lun)
                    sc.ready.isEmpty() -> state(origKey)?.notice = Notice(Severity.Warning, getString(R.string.notice_lun_none_ready), getString(R.string.notice_lun_none_ready_detail, sc.lunCount))
                    else -> state(origKey)?.notice = Notice(Severity.Info, getString(R.string.notice_lun_choose), getString(R.string.notice_lun_choose_detail))
                }
            }
        }.start()
    }

    private fun openLun(dev: UsbDevice, info: UsbDeviceInfo, lun: Int) {
        val key = UsbSlotKey.of(dev.deviceName, lun)
        val d = state(key) ?: return
        Thread {
            var notice: Notice? = null
            var table: PartitionTable? = null
            var s: UsbSession? = null
            try {
                s = sessions.open(info, lun)
                table = s.partitionTable()
                updateOps(key, "after open+partition scan")
            } catch (e: UsbSessionException) {
                notice = when (e.kind) {
                    UsbSessionException.Kind.AlreadyOpen -> Notice(Severity.Info, getString(R.string.notice_usb_already_open))
                    UsbSessionException.Kind.Closing -> Notice(Severity.Info, getString(R.string.notice_usb_closing))
                    UsbSessionException.Kind.WriteNotAllowed -> Notice(Severity.Error, getString(R.string.notice_usb_write_disabled))
                    UsbSessionException.Kind.ClosedWhileOpening -> Notice(Severity.Warning, getString(R.string.notice_usb_closed_while_opening))
                    UsbSessionException.Kind.LimitReached -> Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.message)
                }
            } catch (e: NativeException) {
                notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.message ?: "native error")
            } catch (e: Exception) {
                notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.javaClass.simpleName)
            }
            if (s != null && table == null) sessions.close(key)   // 開けたがパーティションを読めなかった: 画面に出ないセッションを残さない
            val opened = s; val tbl = table; val n = notice
            // LUN 一覧を使っていない (LUN 1 つの) デバイスで開けなかったら、調査のために保っていた接続を OS に返す (従来どおり、失敗したら何も確保しない)
            if (opened == null && n != null && !lunScans.containsKey(dev.deviceName) && !sessions.isDeviceBusy(dev.deviceName)) sessions.releaseDevice(dev.deviceName)
            ui {
                val st = state(key)
                if (st != null) {
                    st.busy = null
                    st.notice = n
                    if (tbl != null && opened != null) {   // 二重オープンの拒否のときは、すでに開いているセッションの表示を壊さない (ここには来ない)
                        st.open = true
                        st.writable = opened.writable
                        st.writeMode = sessions.writeMode(key)
                        st.published = emptyList()
                        st.table = tbl
                        st.tuningSaved = describeTuning(opened)
                        // 回転ディスクか。回転ディスク以外は、ユーザーが確認済みの種別と一致しない限りスピンダウンの操作を隠し、自動停止も働かない
                        st.mediaKind = opened.mediaKind
                        st.spinDownUnlocked = sessions.spinDownAllowed(key)
                        if (!st.spinDownUnlocked) st.idleSpinDownMin = 0   // 保存値は無視して「しない」と表示 (保存値そのものは消さない)
                    }
                }
                refreshUsb()
            }
        }.start()
    }

    /** 複数 LUN のデバイスの LUN 一覧を取り直す (カードを入れ替えた後)。開いている LUN には何も送らない。 */
    override fun rescanLuns(key: String) {
        val dev = usbManager.deviceList[UsbSlotKey.deviceOf(key)] ?: return
        val d = state(key) ?: return
        if (d.busy != null) return
        if (!usbManager.hasPermission(dev)) return
        d.busy = getString(R.string.busy_rescanning_luns)
        d.notice = null
        scanThenOpen(dev, infoOf(dev), key, thenOpen = false)
    }

    /** 複数 LUN のデバイスを OS に返す: LUN 一覧のために保っていた接続を解放する。どれかの LUN が開いている最中は何もしない。 */
    override fun releaseUsb(key: String) {
        val name = UsbSlotKey.deviceOf(key)
        if (model.usbDevices.any { it.deviceName == name && (it.open || it.busy != null) }) return
        Thread {
            sessions.releaseDevice(name)
            ui {
                lunScans.remove(name)
                refreshUsb()
                state(UsbSlotKey.of(name, 0))?.notice = Notice(Severity.Success, getString(R.string.notice_lun_released))
            }
        }.start()
    }

    /** USB 上のパーティションをマウントして SAF に公開する (開いたセッションが書き込み可なら、新規作成のみ書き込み可。同じ USB の別のパーティションも同時に公開できる)。 */
    override fun publishPartition(key: String, startBytes: Long, sizeBytes: Long, label: String) = publishPartition(key, startBytes, sizeBytes, label, allowNoload = false)

    /** ext3/ext4 のジャーナル未再生を承知のうえで (確認ダイアログの後)、読み取り専用でマウントする。書き込みは常に不可。 */
    override fun publishPartitionNoload(key: String, startBytes: Long, sizeBytes: Long, label: String) { state(key)?.noloadPrompt = null; publishPartition(key, startBytes, sizeBytes, label, allowNoload = true) }
    override fun cancelNoload(key: String) { state(key)?.noloadPrompt = null }

    private fun publishPartition(key: String, startBytes: Long, sizeBytes: Long, label: String, allowNoload: Boolean) {
        val sess = sessions.session(key) ?: return
        val d = state(key) ?: return
        val mode = sessions.writeMode(key)
        d.busy = getString(if (sess.writable) R.string.busy_mounting_write else R.string.busy_mounting_read_only)
        d.notice = null
        Thread {
            var notice: Notice? = null
            var pub: PublishedVolume? = null
            var pendingNoload: NoloadPrompt? = null
            try {
                val v = sess.mountNtfs(startBytes, sizeBytes, writable = sess.writable && !allowNoload, createOnly = mode != WriteMode.Full, allowNoload = allowNoload)
                val vk = VolumeRegistry.usbKey(key, startBytes)
                VolumeRegistry.addVolume(label, vk, v, deviceName = key)
                if (sessions.session(key) !== sess) {   // マウントしている間にこのデバイスが閉じられた/抜かれた: 登録を取り消す
                    VolumeRegistry.removeByKey(vk)
                    throw IllegalStateException("device closed")
                }
                notifyRoots()
                updateOps(key, "after ntfs mount")
                android.util.Log.i("usbmem", "ntfs mount [$key]: sessionWritable=${sess.writable} volumeWritable=${v.writable} createOnly=${v.createOnly}")
                val roText = getString(R.string.mode_read_only)
                if (v.journalIgnored) {
                    pub = PublishedVolume(label, getString(R.string.mode_read_only_journal_ignored), false, volumeKey = vk, startBytes = startBytes)
                    notice = Notice(Severity.Warning, getString(R.string.notice_journal_ignored), getString(R.string.notice_journal_ignored_detail))
                } else if (!sess.writable) pub = PublishedVolume(label, roText, false, volumeKey = vk, startBytes = startBytes)
                else if (v.writable) pub = PublishedVolume(label, getString(if (v.createOnly) R.string.mode_create_only else R.string.mode_full), true, fullWrite = !v.createOnly, canLink = v.fsName == "ext2" || v.fsName == "ext4" || v.fsName == "btrfs" || v.fsName == "f2fs" || v.fsName == "xfs", volumeKey = vk, startBytes = startBytes)
                else {
                    pub = PublishedVolume(label, roText, false, volumeKey = vk, startBytes = startBytes)
                    notice = Notice(Severity.Warning, getString(R.string.notice_published_read_only), getString(R.string.notice_published_read_only_detail))
                }
            } catch (e: NativeException) {
                android.util.Log.w("usbmem", "ntfs mount failed [$key]: code=${e.code} ${e.message}")
                if (e.code == 117 && !allowNoload) pendingNoload = NoloadPrompt(startBytes, sizeBytes, label)   // ジャーナル未再生: 確認すれば読み取り専用で開ける
                notice = if (fsRefusalNotice(e) != null) fsRefusalNotice(e)!!   // ext3/ext4 の意図的な拒否 (転送設定とは無関係)
                else if (sess.tuningIndex >= 0) {   // 保存した転送設定が原因かもしれない → 既定に戻す (次の最適化で、その設定以上は試さない)
                    TransferTuning.markFailed(this, sess.vendorId, sess.productId, sess.tuningIndex)
                    TransferTuning.reset(this, sess.vendorId, sess.productId)
                    Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed_with_reason, e.message ?: ""), getString(R.string.notice_ntfs_open_failed_tuning_reset))
                } else Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.message)
            } catch (e: Exception) {
                notice = Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.javaClass.simpleName)
            }
            val p = pub; val n = notice; val np = pendingNoload
            ui {
                val st = state(key)
                if (st != null) {
                    st.busy = null
                    if (p != null && st.open) st.published = st.published.filter { it.volumeKey != p.volumeKey } + p
                    st.notice = n; st.noloadPrompt = np
                }
            }
        }.start()
    }

    /** 公開中のボリューム 1 つだけをアンマウントする (書き込みの確定を含む)。USB は開いたまま、同じ USB の他のボリュームも他の USB もそのまま。 */
    override fun unpublish(key: String, volumeKey: String) {
        val d = state(key) ?: return
        d.busy = getString(R.string.busy_closing_usb)
        d.notice = null
        Thread {
            var notice: Notice? = null
            try { VolumeRegistry.removeByKey(volumeKey) } catch (e: Exception) { notice = Notice(Severity.Error, getString(R.string.notice_usb_open_failed), e.javaClass.simpleName) }
            notifyRoots()
            ui { state(key)?.let { it.busy = null; it.published = it.published.filter { p -> p.volumeKey != volumeKey }; it.notice = notice } }
        }.start()
    }

    private fun settingsPrefs() = getSharedPreferences("usbmem_settings", MODE_PRIVATE)

    /** アイドル自動スピンダウンの設定 (分、0 = オフ)。**このデバイスだけ**の設定で、機種 (VID:PID) ごとに保存する。開いていれば直ちに反映する。 */
    override fun setIdleSpinDown(key: String, minutes: Int) {
        val d = state(key) ?: return
        if (d.open && !d.spinDownUnlocked) return   // 回転ディスク以外では、確認するまで設定できない
        d.idleSpinDownMin = minutes
        runCatching { settingsPrefs().edit().putInt(idleKey(d.settingsId), minutes).apply() }
        sessions.setIdleSpinDownMinutes(key, minutes)
        if (minutes == 0) d.diskIdleStopped = false
    }

    /** 回転ディスク以外 (フラッシュ/SSD・種別不明) でスピンダウンを使うと、ユーザーが確認した。種別ごと・機種ごとに覚える。自動停止は「しない」から始める。 */
    override fun unlockSpinDown(key: String) {
        val d = state(key) ?: return
        val kind = d.mediaKind.kind
        if (!d.open || kind == MediaKind.Rotational) return
        runCatching { settingsPrefs().edit().putString(spinAckKey(d.settingsId), kind.name).putInt(idleKey(d.settingsId), 0).apply() }
        sessions.setSpinDownConfirmed(key, kind)
        sessions.setIdleSpinDownMinutes(key, 0)
        d.idleSpinDownMin = 0
        d.spinDownUnlocked = true
    }

    /** 確認を取り消して、スピンダウンの操作を閉じる (自動停止も止める)。停止中は閉じない (再開の操作を残すため)。 */
    override fun lockSpinDown(key: String) {
        val d = state(key) ?: return
        if (d.mediaKind.kind == MediaKind.Rotational || d.diskStopped) return
        runCatching { settingsPrefs().edit().remove(spinAckKey(d.settingsId)).putInt(idleKey(d.settingsId), 0).apply() }
        sessions.setSpinDownConfirmed(key, null)
        sessions.setIdleSpinDownMinutes(key, 0)
        d.idleSpinDownMin = 0
        d.diskIdleStopped = false
        d.spinDownUnlocked = false
    }

    /**
     * ディスクを止める: このデバイスの公開中のボリュームを閉じて (アンマウント + 書き込みの確定)、SCSI START STOP UNIT で回転を止める。
     * セッション (USB の確保) は保つ: 閉じると OS の usb-storage が戻って FAT をマウントし、ディスクがまた動くため。止めた後は、USB を抜く。他のデバイスは止めない。
     */
    override fun stopDisk(key: String) {
        val s = sessions.session(key) ?: return
        val d = state(key) ?: return
        if (!d.spinDownUnlocked) return   // 回転ディスク以外は、確認してからだけ
        d.busy = getString(R.string.busy_stopping_disk)
        d.notice = null
        Thread {
            var notice: Notice
            var stopped = false
            VolumeRegistry.removeByDevice(key)   // アンマウント + 書き込みの確定 (このデバイスのボリュームだけ。ネストした子も)
            notifyRoots()
            try {
                s.startStop(false)
                stopped = true
                notice = Notice(Severity.Success, getString(R.string.notice_disk_stopped), getString(R.string.notice_disk_stopped_detail))
            } catch (e: NativeException) {
                android.util.Log.w("usbmem", "stop disk failed [$key]: code=${e.code} ${e.message}")
                notice = if (e.code == 9) Notice(Severity.Warning, getString(R.string.notice_disk_stop_unsupported), getString(R.string.notice_disk_stop_unsupported_detail))
                else Notice(Severity.Error, getString(R.string.notice_disk_stop_failed), e.message)
            }
            updateOps(key, "after stop disk")
            ui { state(key)?.let { it.busy = null; it.published = emptyList(); it.diskStopped = stopped; it.notice = notice } }
        }.start()
    }

    override fun resumeDisk(key: String) {
        val s = sessions.session(key) ?: return
        val d = state(key) ?: return
        d.busy = getString(R.string.busy_resuming_disk)
        Thread {
            var notice: Notice
            var ok = false
            try { s.startStop(true); ok = true; notice = Notice(Severity.Success, getString(R.string.notice_disk_resumed)) }
            catch (e: NativeException) { notice = Notice(Severity.Error, getString(R.string.notice_disk_stop_failed), e.message) }
            updateOps(key, "after resume disk")
            ui { state(key)?.let { it.busy = null; if (ok) it.diskStopped = false; it.notice = notice } }
        }.start()
    }

    /** このデバイスを閉じる (公開中のボリュームのアンマウントと書き込みの確定 → USB の解放)。他のデバイスは開いたまま。 */
    override fun closeUsb(key: String) {
        val d = state(key) ?: return
        d.busy = getString(R.string.busy_closing_usb)
        Thread {
            sessions.close(key, CloseReason.User)
            ui {
                state(key)?.let {
                    it.resetAfterClose()
                    it.notice = Notice(Severity.Success, getString(R.string.notice_usb_closed), getString(R.string.notice_usb_closed_detail))
                }
                refreshUsb()
            }
        }.start()
    }

    private fun notifyRoots() = VolumeRegistry.notifyRoots(this)

    // ---- LinkActions (アプリ内のリンク作成)
    private fun linkVolume(volumeKey: String): NtfsVolume = VolumeRegistry.byKey(volumeKey)?.volume ?: throw IllegalStateException("not mounted")
    override fun listDir(volumeKey: String, path: String): List<NtfsEntry> {
        val v = linkVolume(volumeKey)
        return v.list(LinkResolver.physical(v, path, true))
    }
    override fun createLink(volumeKey: String, hard: Boolean, folder: String, name: String, target: String): String? {
        return try {
            val v = linkVolume(volumeKey)
            if (name.contains('/') || name == "." || name == ".." || name.any { it.code < 0x20 }) return getString(R.string.link_err_name)
            val dir = LinkResolver.physical(v, folder, true)
            if (!v.stat(dir).isDir) return getString(R.string.link_err_folder)
            if (hard) {
                val src = LinkResolver.physical(v, target, true)
                if (v.stat(src).isDir) return getString(R.string.link_err_hard_dir)
                v.link(dir, name, src)
            } else v.symlink(dir, name, target)
            null
        } catch (e: NativeException) { getString(R.string.link_err_failed, e.code, e.message ?: "") }
        catch (e: Exception) { getString(R.string.link_err_failed, -1, e.message ?: e.javaClass.simpleName) }
    }
    override fun linkCreated(deviceKey: String, name: String) {
        state(deviceKey)?.notice = Notice(Severity.Success, getString(R.string.link_done, name))
        notifyRoots()
    }

    // ---- DiagnosticsActions
    /** 書き込み自己診断。そのデバイスのマウント中の「すべて許可」ボリュームに、専用の一時フォルダだけを作って検証し、全部消す。既存のファイルには触れない。 */
    override fun writeSelfTest(key: String) {
        val d = state(key) ?: return
        val v = d.published.firstOrNull { it.fullWrite }?.let { VolumeRegistry.byKey(it.volumeKey)?.volume }
        if (v == null || !v.writable || v.createOnly) return
        d.busy = getString(R.string.busy_selftest)
        d.selfTestPassed = null; d.selfTestLog = ""
        Thread {
            val stamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date())
            val r = try { WriteSelfTest.run(v, stamp) } catch (e: Throwable) { null }
            val sb = StringBuilder()
            if (r == null) sb.appendLine(getString(R.string.diag_failed_simple, "exception"))
            else {
                for (s in r.steps) {
                    android.util.Log.i("usbmem", "selftest [$key] step '${s.name}' ok=${s.ok} ${s.ms}ms ${s.detail}")
                    sb.appendLine((if (s.ok) "OK  " else "NG  ") + s.name + " (" + s.ms + " ms)" + if (s.detail.isNotEmpty()) " — " + s.detail else "")
                }
            }
            updateOps(key, "after selftest")
            ui { state(key)?.let { it.busy = null; it.selfTestPassed = r?.passed == true; it.selfTestLog = sb.toString().trimEnd() } }
        }.start()
    }

    override fun refreshOps(key: String) { Thread { updateOps(key, "manual refresh") }.start() }

    /** 読み取り専用の速度計測。転送サイズの組を順に試し、MB/s を表示・ログへ出す。デバイスの先頭付近を読むだけで、何も書かない。 */
    override fun benchmark(key: String) {
        val s = sessions.session(key) ?: return
        val d = state(key) ?: return
        d.busy = getString(R.string.busy_benchmark)
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
                    android.util.Log.i("usbmem", "bench [$key] $line"); sb.appendLine(line)
                }
                val saved = TransferTuning.load(this, s.vendorId, s.productId)
                s.tune(saved?.first ?: UsbStorageSession.READ_CHUNK_KIB, saved?.second ?: UsbStorageSession.READ_MAX_TRANSFER_KIB)   // 保存済み (無ければ既定) へ戻す
            } catch (e: Exception) { sb.appendLine(getString(R.string.diag_failed_simple, e.javaClass.simpleName)) }
            updateOps(key, "after bench")
            ui { state(key)?.let { it.busy = null; it.tuneLog = sb.toString().trim() } }
        }.start()
    }

    /**
     * 読み取り転送サイズの最適化 (読み取りのみ。何も書かない)。小さい設定から順に、同じ区間を読んでハッシュが一致するか + 速度を確かめ、**通った中で最速**を、この端末・この USB 機器用に保存する。
     * 保存した設定は次回から USB を開くたびに使われる (書き込みの転送サイズは変えない)。失敗した設定は記録して二度と試さない。
     * 失敗すると USB が切れて OS が FAT を再マウントすることがある (J9260)。その場合は USB を閉じて、OS の FAT を取り出し、開き直す。
     */
    override fun optimize(key: String) {
        val s = sessions.session(key) ?: return
        val d = state(key) ?: return
        d.busy = getString(R.string.busy_optimize)
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
                    android.util.Log.i("usbmem", "optimize [$key] chunk=${chunk}K cmd=${mt}K: %.1f MB/s".format(mbps))
                    if (mbps > bestMbps * 1.03) { best = i; bestMbps = mbps }
                    TransferTuning.markOk(this, vid, pid, best)   // 途中経過も保存 (試行中の印はここで消える)
                }
                s.tune(ladder[best].first, ladder[best].second)
                sb.appendLine(getString(R.string.diag_opt_saved, ladder[best].first, ladder[best].second, bestMbps))
            } catch (e: Exception) {
                val code = (e as? NativeException)?.code
                android.util.Log.w("usbmem", "optimize failed [$key]: ${e.message}")
                // 失敗した試行は「試行中」の印が残っている → 次回の最適化で失敗として扱われる。ここでも確定させておく
                val trying = TransferTuning.failedFrom(this, vid, pid)
                if (trying < ladder.size) TransferTuning.markFailed(this, vid, pid, trying)
                runCatching { s.tune(ladder[best].first, ladder[best].second) }
                sb.appendLine(getString(R.string.diag_opt_failed, "${e.message}${if (code != null) ", code=$code" else ""}"))
                sb.appendLine(getString(R.string.diag_opt_failed_hint))
                sb.appendLine(getString(R.string.diag_opt_best_so_far, ladder[best].first, ladder[best].second))
            }
            updateOps(key, "after optimize")
            ui { state(key)?.let { it.busy = null; it.tuneLog = sb.toString().trim(); it.tuningSaved = describeTuning(s) } }
        }.start()
    }

    // ---- ImageActions (IO スレッドから呼ばれる)
    override fun loadImage(uri: Uri): ImageResult = try {
        // このアプリのプロバイダが公開しているファイルなら、SAF の fd を経由せず、親ボリュームを直接読む (デッドロック回避)
        VolumeRegistry.ownDocument(uri)?.let { (pid, path) ->
            return ImageResult(label = path.substringAfterLast('/'), table = VolumeRegistry.inspectNested(this, pid, path))
        }
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
    override fun shareImageNoload(uri: Uri): ImageResult = shareImage(uri, writable = false, allowNoload = true)
    override fun shareImage(uri: Uri, writable: Boolean): ImageResult = shareImage(uri, writable, allowNoload = false)

    private fun shareImage(uri: Uri, writable: Boolean, allowNoload: Boolean): ImageResult = try {
        val flags = android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or (if (writable) android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        if (VolumeRegistry.ownDocument(uri) == null) try { contentResolver.takePersistableUriPermission(uri, flags) } catch (_: SecurityException) { }
        val e = VolumeRegistry.addUri(this, uri, writable && !allowNoload, allowNoload)
        notifyRoots()
        if (e.volume.journalIgnored) ImageResult(notice = Notice(Severity.Warning, getString(R.string.notice_journal_ignored), getString(R.string.notice_journal_ignored_detail)))
        else if (!writable) ImageResult(notice = Notice(Severity.Success, getString(R.string.notice_image_shared_read_only, e.label), getString(R.string.notice_image_shared_read_only_detail)))
        else if (e.volume.writable) ImageResult(notice = Notice(Severity.Warning, getString(R.string.notice_image_shared_write, e.label), getString(R.string.notice_image_shared_write_detail)))
        else ImageResult(notice = Notice(Severity.Warning, getString(R.string.notice_image_shared_fallback, e.label), getString(R.string.notice_image_shared_fallback_detail)))
    } catch (e: NativeException) {
        ImageResult(notice = fsRefusalNotice(e) ?: Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.message), noloadUri = if (e.code == 117 && !allowNoload) uri else null)
    } catch (e: Exception) {
        ImageResult(notice = Notice(Severity.Error, getString(R.string.notice_ntfs_open_failed), e.javaClass.simpleName))
    }

    // ---- NestedActions: マウント済みのボリュームの中のイメージを、読み取り専用でマウントする (ループバック。docs/nested-mount.md)
    override fun nestedVolumes(): List<NestedVolumeRow> = VolumeRegistry.all(this).map {
        val depth = try { it.volume.nestDepth() } catch (_: Exception) { 0 }
        NestedVolumeRow(it.id, it.label, it.volume.fsName, depth, canNest = depth < NESTED_MAX_DEPTH)
    }

    override fun nestedList(volumeId: String, path: String): List<NtfsEntry> = try {
        (VolumeRegistry.get(this, volumeId)?.volume?.list(path) ?: emptyList()).sortedWith(compareBy({ !it.isDir }, { it.name.lowercase() }))
    } catch (_: Exception) { emptyList() }

    override fun nestedInspect(volumeId: String, path: String): NestedInspectResult = try {
        NestedInspectResult(table = VolumeRegistry.inspectNested(this, volumeId, path))
    } catch (e: NativeException) {
        NestedInspectResult(notice = nestedNotice(e))
    } catch (e: Exception) {
        NestedInspectResult(notice = Notice(Severity.Error, getString(R.string.notice_nested_open_failed), e.javaClass.simpleName))
    }

    override fun nestedMount(volumeId: String, path: String, startBytes: Long, sizeBytes: Long, label: String, allowNoload: Boolean): NestedMountResult = try {
        val e = VolumeRegistry.mountNested(this, volumeId, path, startBytes, sizeBytes, label, allowNoload)
        notifyRoots()
        if (e.volume.journalIgnored) NestedMountResult(Notice(Severity.Warning, getString(R.string.notice_journal_ignored), getString(R.string.notice_journal_ignored_detail)))
        else NestedMountResult(Notice(Severity.Success, getString(R.string.notice_image_shared_read_only, e.label), getString(R.string.notice_nested_mounted_detail)))
    } catch (e: NativeException) {
        NestedMountResult(nestedNotice(e), noload = e.code == 117 && !allowNoload)
    } catch (e: Exception) {
        NestedMountResult(Notice(Severity.Error, getString(R.string.notice_nested_open_failed), e.javaClass.simpleName))
    }

    private fun nestedNotice(e: NativeException): Notice = fsRefusalNotice(e) ?: when (e.code) {
        40 -> Notice(Severity.Warning, getString(R.string.notice_nested_too_deep), getString(R.string.notice_nested_too_deep_detail, NESTED_MAX_DEPTH))
        22 -> Notice(Severity.Warning, getString(R.string.notice_nested_not_image), getString(R.string.notice_nested_not_image_detail))
        else -> Notice(Severity.Error, getString(R.string.notice_nested_open_failed), e.message)
    }

    /** ext3/ext4 の「マウントしない」理由 (EUCLEAN=117: ジャーナル未回復/破損、EOPNOTSUPP=95: 未対応の機能) を専用の文言にする。それ以外は null。 */
    private fun fsRefusalNotice(e: NativeException): Notice? = when (e.code) {
        117 -> Notice(Severity.Warning, getString(R.string.notice_fs_needs_recovery), getString(R.string.notice_fs_needs_recovery_detail))
        95 -> Notice(Severity.Warning, getString(R.string.notice_fs_unsupported_feature), getString(R.string.notice_fs_unsupported_feature_detail))
        else -> null
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
        refreshUsb()   // 止まっている間に抜かれた/閉じられたデバイスも、ここで画面とセッションを合わせる
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
        // 終了するとき (戻るボタンなど) だけ、全部のデバイスを閉じる。システムによる作り直しでは、セッションはプロセスが持っているので閉じない
        if (isFinishing) sessions.closeAll()
        sessions.removeListener(sessionListener)
        super.onDestroy()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        sessions.addListener(sessionListener)
        setContent { AppTheme { AppScaffold(model, this, this, this) } }
    }
}
