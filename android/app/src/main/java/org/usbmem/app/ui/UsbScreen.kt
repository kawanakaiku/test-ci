package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.usbmem.app.BuildConfig
import org.usbmem.app.MediaBasis
import org.usbmem.app.MediaHint
import org.usbmem.app.MediaKind
import org.usbmem.app.MediaKindInfo
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.UsbDeviceNaming
import org.usbmem.app.UsbLunInfo
import org.usbmem.app.UsbLunState
import org.usbmem.app.WriteMode
import org.usbmem.app.formatBytes

/** USB 画面で起こせる操作。実処理は MainActivity。どれも [key] (スロット名: UsbDevice.deviceName + LUN) のスロットだけが対象で、他のデバイス・他の LUN には影響しない。 */
interface UsbActions : LinkActions {
    fun openStorageSettings()
    fun openUsb(key: String)
    /** 複数 LUN のデバイスの LUN 一覧を取り直す (カードを入れ替えた後)。開いている LUN は調べない。 */
    fun rescanLuns(key: String)
    /** 複数 LUN のデバイスを OS に返す (LUN 一覧を閉じ、接続を解放する)。どの LUN も開いていないときだけ。 */
    fun releaseUsb(key: String)
    fun publishPartition(key: String, startBytes: Long, sizeBytes: Long, label: String)
    fun publishPartitionNoload(key: String, startBytes: Long, sizeBytes: Long, label: String)
    fun cancelNoload(key: String)
    /** 公開中のボリューム 1 つだけをアンマウントする (USB は開いたまま)。 */
    fun unpublish(key: String, volumeKey: String)
    fun closeUsb(key: String)
    fun stopDisk(key: String)
    fun resumeDisk(key: String)
    fun setIdleSpinDown(key: String, minutes: Int)
    /** 回転ディスク以外 (フラッシュ/SSD・種別不明) でスピンダウンを使う確認が取れた。 */
    fun unlockSpinDown(key: String)
    fun lockSpinDown(key: String)
    fun setWriteMode(key: String, mode: WriteMode)
}

@Composable
fun UsbScreen(m: AppModel, a: UsbActions, modifier: Modifier = Modifier) {
    val anyOpen = m.usbDevices.any { it.open }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        m.usbNotice?.let { Banner(it) }

        // ---- デバイス (1 本ごとのカード。カードのタイトルが、どのデバイスの操作かを示す)
        if (m.usbDevices.isEmpty()) {
            Banner(Notice(Severity.Info, stringResource(R.string.usb_none_title), stringResource(R.string.usb_none_detail)))
        } else {
            if (m.usbDevices.size >= 2) Banner(Notice(Severity.Info, stringResource(R.string.usb_hub_hint)))
            m.usbDevices.forEach { d -> key(d.key) { DeviceCard(d, a, canRelease = d.lunInfo != null && m.usbDevices.none { it.deviceName == d.deviceName && (it.open || it.busy != null) }) } }
        }

        // ---- 手順 (どれも開いていないときだけ)
        if (!anyOpen) {
            SectionCard(stringResource(R.string.usb_guide_title)) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Step(1, stringResource(R.string.usb_guide_1_title), stringResource(R.string.usb_guide_1_body), done = m.usbDevices.isNotEmpty() && m.usbDevices.all { it.osBlocked == 0 })
                    Step(2, stringResource(R.string.usb_guide_2_title), stringResource(R.string.usb_guide_2_body))
                    Step(3, stringResource(R.string.usb_guide_3_title), stringResource(R.string.usb_guide_3_body))
                    Step(4, stringResource(R.string.usb_guide_4_title), stringResource(R.string.usb_guide_4_body))
                    Step(5, stringResource(R.string.usb_guide_5_title), stringResource(R.string.usb_guide_5_body))
                }
            }
        }
    }
}

private fun modeLabel(mode: WriteMode) = when (mode) { WriteMode.ReadOnly -> R.string.common_mode_read_only; WriteMode.CreateOnly -> R.string.common_mode_create_only; WriteMode.Full -> R.string.mode_full_short }

/** USB 1 本分のカード。書き込みモード・開閉・パーティションと公開中ボリューム・ディスクの電源が、すべてこのデバイスのもの。 */
@Composable
private fun DeviceCard(d: UsbDeviceState, a: UsbActions, canRelease: Boolean = false) {
    var linkFor by remember { mutableStateOf<PublishedVolume?>(null) }
    var confirmClose by remember { mutableStateOf(false) }
    var confirmFull by remember { mutableStateOf(false) }
    val idle = d.busy == null
    val li = d.lunInfo
    val noMedia = li != null && !li.openable   // 複数 LUN の機器で、メディアなし (など) の LUN: 開けない
    SectionCard(
        d.title, subtitle = "${d.vidPid}  ·  ${d.shortId}",
        trailing = { if (d.open) Chip(stringResource(R.string.usb_connected), severity = Severity.Success) else Chip(stringResource(R.string.usb_not_connected)) },
    ) {
        // ---- 複数 LUN: この LUN の状態 (メディアなしは灰色で、開けない)
        if (li != null && !d.open) {
            Text(
                lunStatusText(li), style = MaterialTheme.typography.bodyMedium,
                color = if (li.openable) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        // ---- 書き込みモード (この USB を開く前に決める。ビルドで書き込みが無効なら読み取り専用のみ)
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.usb_mode_title), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
                Chip(stringResource(modeLabel(d.writeMode)), severity = if (d.writeMode == WriteMode.ReadOnly) Severity.Success else Severity.Warning)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WriteMode.entries.forEach { mode ->
                    FilterChip(
                        selected = d.writeMode == mode,
                        onClick = { if (mode == WriteMode.Full && d.writeMode != WriteMode.Full) confirmFull = true else a.setWriteMode(d.key, mode) },
                        enabled = !d.open && idle && (mode == WriteMode.ReadOnly || BuildConfig.WRITE_SUPPORTED),
                        label = { Text(stringResource(modeLabel(mode))) },
                    )
                }
            }
            Text(stringResource(if (!BuildConfig.WRITE_SUPPORTED) R.string.usb_write_unavailable else when (d.writeMode) { WriteMode.ReadOnly -> R.string.usb_mode_desc_read_only; WriteMode.CreateOnly -> R.string.usb_write_desc; WriteMode.Full -> R.string.usb_mode_desc_full }), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // ---- OS のマウント警告 (この USB を開くのを妨げているときだけ)
        if (!d.open && d.osBlocked > 0) {
            Banner(
                Notice(Severity.Warning, stringResource(R.string.usb_os_mounted_title, d.osBlocked), stringResource(R.string.usb_os_mounted_detail)),
                action = { OutlinedButton(onClick = a::openStorageSettings) { Text(stringResource(R.string.usb_open_storage_settings)) } },
            )
        }

        // ---- 開く (メディアなしの LUN は開けない)
        if (!d.open) {
            Button(onClick = { a.openUsb(d.key) }, enabled = idle && !noMedia, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(when (d.writeMode) { WriteMode.ReadOnly -> R.string.usb_open_read_only; WriteMode.CreateOnly -> R.string.usb_open_write; WriteMode.Full -> R.string.usb_open_full }))
            }
        }
        // ---- 複数 LUN: カードの入れ替え後に LUN 一覧を取り直す / 全部の LUN を使い終えたら OS に返す
        if (li != null && !d.open) {
            OutlinedButton(onClick = { a.rescanLuns(d.key) }, enabled = idle, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_lun_rescan)) }
            if (canRelease) TextButton(onClick = { a.releaseUsb(d.key) }, enabled = idle, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_lun_release)) }
        }

        if (d.busy != null) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(d.busy ?: "", style = MaterialTheme.typography.bodySmall)
            }
        }
        d.notice?.let { Banner(it) }

        val t = d.table
        if (d.open && t != null) {
            // ---- ディスクの種別 (回転ディスクか、フラッシュ/SSD か。開くとき 1 度だけ判別)
            MediaKindRow(d.mediaKind)

            // ---- パーティション一覧と公開中ボリューム
            PartitionList(d, t, a, onLink = { linkFor = it })

            // ---- ディスクの電源 (スピンダウン)。書き込み確定のために公開中のボリュームを閉じてから止める
            PowerSection(d, a)

            if (d.published.isNotEmpty()) Text(stringResource(R.string.usb_published_warning), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            FilledTonalButton(onClick = { if (d.writable) confirmClose = true else a.closeUsb(d.key) }, enabled = idle, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(if (d.published.isNotEmpty()) R.string.usb_close_safely else R.string.usb_close_without_publish))
            }
        }
    }
    if (confirmFull) {
        ConfirmDialog(
            title = stringResource(R.string.usb_full_dialog_title),
            text = stringResource(R.string.usb_full_dialog_text),
            confirmLabel = stringResource(R.string.usb_full_dialog_confirm),
            onConfirm = { confirmFull = false; a.setWriteMode(d.key, WriteMode.Full) },
            onDismiss = { confirmFull = false },
        )
    }
    d.noloadPrompt?.let { p ->
        ConfirmDialog(
            title = stringResource(R.string.noload_dialog_title),
            text = stringResource(R.string.noload_dialog_text),
            confirmLabel = stringResource(R.string.noload_dialog_confirm),
            onConfirm = { a.publishPartitionNoload(d.key, p.startBytes, p.sizeBytes, p.label) },
            onDismiss = { a.cancelNoload(d.key) },
        )
    }
    linkFor?.let { pub ->
        LinkDialog(a, volumeKey = pub.volumeKey, allowHard = pub.fullWrite, onDone = { linkFor = null; a.linkCreated(d.key, it) }, onDismiss = { linkFor = null })
    }
    if (confirmClose) {
        ConfirmDialog(
            title = stringResource(R.string.usb_close_dialog_title),
            text = stringResource(R.string.usb_close_dialog_text),
            confirmLabel = stringResource(R.string.usb_close_dialog_confirm),
            onConfirm = { confirmClose = false; a.closeUsb(d.key) },
            onDismiss = { confirmClose = false },
        )
    }
}

/** 複数 LUN の機器の LUN 1 つの状態の文言 (例: 「メディアなし」「15.6 GB」)。 */
@Composable
private fun lunStatusText(li: UsbLunInfo): String = when (li.state) {
    UsbLunState.Ready -> stringResource(R.string.usb_lun_ready, formatBytes(li.sizeBytes))
    UsbLunState.NoMedia -> stringResource(R.string.usb_lun_no_media)
    UsbLunState.NotReady -> stringResource(R.string.usb_lun_not_ready)
    UsbLunState.Unsupported -> stringResource(R.string.usb_lun_unsupported)
    UsbLunState.Error -> stringResource(R.string.usb_lun_error, li.note)
    UsbLunState.Skipped -> stringResource(R.string.usb_lun_skipped)
}

/** 種別の表示用の文言 (例: 「HDD 7200rpm」「SSD/フラッシュ」「種別不明 (USB メモリの可能性)」)。 */
@Composable
fun mediaKindLabel(k: MediaKindInfo): String = when (k.kind) {
    MediaKind.Rotational -> if (k.rpm > 0) stringResource(R.string.usb_media_hdd_rpm, k.rpm) else stringResource(R.string.usb_media_hdd)
    MediaKind.NonRotational -> stringResource(R.string.usb_media_ssd)
    MediaKind.Unknown -> when (k.hint) {
        MediaHint.RemovableNoVpd -> stringResource(R.string.usb_media_unknown_usbmem)
        MediaHint.UnmapSupported -> stringResource(R.string.usb_media_unknown_unmap)
        MediaHint.None -> stringResource(R.string.usb_media_unknown)
    }
}

@Composable
fun mediaBasisLabel(k: MediaKindInfo): String = when (k.basis) {
    MediaBasis.VpdBlockDeviceChars -> stringResource(R.string.usb_media_basis_vpd)
    MediaBasis.AtaInfoVpd -> stringResource(R.string.usb_media_basis_ata_info)
    MediaBasis.AtaPassThrough12, MediaBasis.AtaPassThrough16 -> stringResource(R.string.usb_media_basis_sat)
    MediaBasis.None -> stringResource(R.string.usb_media_basis_none)
}

@Composable
private fun MediaKindRow(k: MediaKindInfo) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.usb_media_title), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            Chip(mediaKindLabel(k), severity = if (k.kind == MediaKind.Unknown) Severity.Warning else Severity.Info)
        }
        Text(mediaBasisLabel(k), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * ディスクの電源。回転ディスクと判別できたときだけ、停止/再開とアイドル自動停止をそのまま出す。
 * フラッシュ/SSD・種別不明は、既定で折りたたんで警告だけを出し、確認ダイアログの後にだけ使える (自動停止の既定は常に「しない」)。
 */
@Composable
private fun PowerSection(d: UsbDeviceState, a: UsbActions) {
    val rotational = d.mediaKind.isRotational
    var confirmUnlock by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.usb_power_title), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            if (d.diskStopped) Chip(stringResource(R.string.usb_power_stopped_chip), severity = Severity.Warning) else if (d.diskIdleStopped) Chip(stringResource(R.string.usb_idle_stopped_chip), severity = Severity.Warning)
        }
        if (!rotational) {
            val flash = d.mediaKind.kind == MediaKind.NonRotational
            Banner(Notice(
                Severity.Warning,
                stringResource(if (flash) R.string.usb_power_flash_warning_title else R.string.usb_power_unknown_warning_title),
                stringResource(if (flash) R.string.usb_power_flash_warning else R.string.usb_power_unknown_warning),
            ))
            when (d.mediaKind.hint) {
                MediaHint.RemovableNoVpd -> Text(stringResource(R.string.usb_power_unknown_hint_usbmem), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                MediaHint.UnmapSupported -> Text(stringResource(R.string.usb_power_unknown_hint_unmap), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                MediaHint.None -> {}
            }
        }
        if (!rotational && !d.spinDownUnlocked) {
            OutlinedButton(onClick = { confirmUnlock = true }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_power_unlock_button)) }
        } else {
            if (d.diskStopped) {
                Text(stringResource(R.string.usb_power_stopped_desc), style = MaterialTheme.typography.bodyMedium)
                FilledTonalButton(onClick = { a.resumeDisk(d.key) }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_power_resume)) }
            } else {
                Text(stringResource(R.string.usb_power_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedButton(onClick = { a.stopDisk(d.key) }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_power_stop)) }
                Text(stringResource(R.string.usb_idle_title), style = MaterialTheme.typography.titleSmall)
                Text(stringResource(R.string.usb_idle_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(0, 5, 10, 30).forEach { min ->
                        FilterChip(
                            selected = d.idleSpinDownMin == min,
                            onClick = { a.setIdleSpinDown(d.key, min) },
                            label = { Text(if (min == 0) stringResource(R.string.usb_idle_off) else stringResource(R.string.usb_idle_minutes, min)) },
                        )
                    }
                }
                if (!rotational) TextButton(onClick = { a.lockSpinDown(d.key) }, enabled = d.busy == null) { Text(stringResource(R.string.usb_power_lock_button)) }
            }
        }
    }
    if (confirmUnlock) {
        ConfirmDialog(
            title = stringResource(R.string.usb_power_unlock_dialog_title),
            text = stringResource(R.string.usb_power_unlock_dialog_text),
            confirmLabel = stringResource(R.string.usb_power_unlock_dialog_confirm),
            onConfirm = { confirmUnlock = false; a.unlockSpinDown(d.key) },
            onDismiss = { confirmUnlock = false },
        )
    }
}

private fun fsMountable(fs: String) = fs == "ntfs" || fs == "exfat" || fs == "ext2" || fs == "ext3" || fs == "ext4" || fs == "fat12" || fs == "fat16" || fs == "fat32" || fs == "iso9660" || fs == "udf" || fs == "squashfs" || fs == "erofs" || fs == "btrfs" || fs == "f2fs" || fs == "xfs"

/** パーティション一覧。公開中の行にはマウントの状態とアンマウント・リンク作成が出る。1 本の USB の複数のパーティションを同時に公開できる。 */
@Composable
private fun PartitionList(d: UsbDeviceState, t: PartitionTable, a: UsbActions, onLink: (PublishedVolume) -> Unit) {
    val canPublish = d.busy == null && !d.diskStopped
    val writeMode = if (d.writable) d.writeMode else WriteMode.ReadOnly
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(stringResource(R.string.usb_card_open_title), style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.common_partitions_subtitle, d.title, t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        t.partitions.forEach { p ->
            val pub = if (t.wholeDeviceVolume) null else d.published.firstOrNull { it.startBytes == p.startBytes }
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                    Chip(p.fs.uppercase())
                    if (pub != null) Chip(pub.modeText, severity = if (pub.writable) Severity.Warning else Severity.Success)
                }
                Text(stringResource(if (p.extendedContainer) R.string.common_partition_detail_extended else R.string.common_partition_detail, formatBytes(p.sizeBytes), p.typeName, p.startLba), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (pub != null) {
                    PublishedActions(d, pub, a, onLink)
                } else if (fsMountable(p.fs)) {
                    // ISO 9660 / UDF / squashfs / erofs は読み取り専用 (書き込みは未対応)。NTFS・exFAT・FAT12/16/32・ext2・ext3・ext4・btrfs・f2fs・xfs は、このデバイスの書き込みモードに従う (xfs は CreateOnly の範囲だけ: docs/xfs-write.md) (ext3/ext4 は jbd2: docs/ext4-write.md、btrfs は COW: docs/btrfs-write.md、f2fs は docs/f2fs-write.md)
                    val mode = if (p.fs == "ntfs" || p.fs == "exfat" || p.fs == "fat12" || p.fs == "fat16" || p.fs == "fat32" || p.fs == "ext2" || p.fs == "ext3" || p.fs == "ext4" || p.fs == "btrfs" || p.fs == "f2fs" || p.fs == "xfs") writeMode else WriteMode.ReadOnly
                    Button(onClick = { a.publishPartition(d.key, p.startBytes, p.sizeBytes, UsbDeviceNaming.volumeLabel(d.productName, d.shortId, p.index, if (d.lunInfo != null) d.lun else null)) }, enabled = canPublish, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(when (mode) { WriteMode.ReadOnly -> R.string.usb_publish_read_only; WriteMode.CreateOnly -> R.string.usb_publish_write; WriteMode.Full -> R.string.usb_publish_full }))
                    }
                }
            }
        }
        if (t.wholeDeviceVolume) {
            val pub = d.published.firstOrNull { it.startBytes == 0L }
            if (pub != null) PublishedActions(d, pub, a, onLink)
            else OutlinedButton(onClick = { a.publishPartition(d.key, 0, t.diskBytes, UsbDeviceNaming.volumeLabel(d.productName, d.shortId, null, if (d.lunInfo != null) d.lun else null)) }, enabled = canPublish, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_publish_whole_device)) }
        }
        t.diagnostics.forEach { x -> Text("${x.severity}: ${x.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp)) }
    }
}

@Composable
private fun PublishedActions(d: UsbDeviceState, pub: PublishedVolume, a: UsbActions, onLink: (PublishedVolume) -> Unit) {
    Text(stringResource(R.string.usb_published_desc) + "  (" + pub.label + ")", style = MaterialTheme.typography.bodySmall)
    if (pub.canLink) OutlinedButton(onClick = { onLink(pub) }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.link_button)) }
    OutlinedButton(onClick = { a.unpublish(d.key, pub.volumeKey) }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_unmount)) }
}
