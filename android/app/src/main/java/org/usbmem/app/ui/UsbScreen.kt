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
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes

/** USB 画面で起こせる操作。実処理は MainActivity。 */
interface UsbActions {
    fun openStorageSettings()
    fun openUsb(key: String)
    fun publishPartition(startBytes: Long, sizeBytes: Long, label: String)
    fun closeUsb()
    fun stopDisk()
    fun resumeDisk()
    fun setIdleSpinDown(minutes: Int)
    fun setAllowWrite(v: Boolean)
}

@Composable
fun UsbScreen(m: AppModel, a: UsbActions, modifier: Modifier = Modifier) {
    var confirmClose by remember { mutableStateOf(false) }
    val isOpen = m.usbOpenKey != null
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ---- モード (USB を開く前に決める)
        SectionCard(
            stringResource(R.string.usb_mode_title),
            subtitle = stringResource(R.string.usb_mode_subtitle),
            trailing = { Chip(stringResource(if (m.allowUsbWrite) R.string.common_mode_create_only else R.string.common_mode_read_only), severity = if (m.allowUsbWrite) Severity.Warning else Severity.Success) },
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(R.string.common_write_allow), style = MaterialTheme.typography.bodyLarge)
                    Text(stringResource(R.string.usb_write_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = m.allowUsbWrite, onCheckedChange = { a.setAllowWrite(it) }, enabled = !isOpen && m.busy == null)
            }
        }

        // ---- 接続前の案内 / OS のマウント警告
        if (!isOpen && m.osMountedUsb > 0) {
            Banner(
                Notice(Severity.Warning, stringResource(R.string.usb_os_mounted_title, m.osMountedUsb), stringResource(R.string.usb_os_mounted_detail)),
                action = { OutlinedButton(onClick = a::openStorageSettings) { Text(stringResource(R.string.usb_open_storage_settings)) } },
            )
        }

        // ---- デバイス
        if (m.usbDevices.isEmpty()) {
            Banner(Notice(Severity.Info, stringResource(R.string.usb_none_title), stringResource(R.string.usb_none_detail)))
        } else {
            m.usbDevices.forEach { d ->
                val open = m.usbOpenKey == d.key
                SectionCard(d.title, subtitle = d.vidPid, trailing = { if (open) Chip(stringResource(R.string.usb_connected), severity = Severity.Success) }) {
                    if (!open) {
                        Button(onClick = { a.openUsb(d.key) }, enabled = m.busy == null && !isOpen, modifier = Modifier.fillMaxWidth()) {
                            Text(stringResource(if (m.allowUsbWrite) R.string.usb_open_write else R.string.usb_open_read_only))
                        }
                    }
                }
            }
        }

        if (m.busy != null) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(m.busy ?: "", style = MaterialTheme.typography.bodySmall)
            }
        }
        m.usbNotice?.let { Banner(it) }

        // ---- 公開中
        val pub = m.published
        if (pub != null) {
            SectionCard(stringResource(R.string.usb_published_title), subtitle = pub.label, trailing = { Chip(pub.modeText, severity = if (pub.writable) Severity.Warning else Severity.Success) }) {
                Text(stringResource(R.string.usb_published_desc), style = MaterialTheme.typography.bodyMedium)
                if (pub.writable) Text(stringResource(R.string.usb_published_warning), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { if (m.usbWritable) confirmClose = true else a.closeUsb() }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_close_safely)) }
            }
        }

        // ---- ディスクの電源 (スピンダウン)。開いていて、書き込み確定のために公開中のボリュームを閉じてから止める
        if (isOpen) {
            SectionCard(stringResource(R.string.usb_power_title), subtitle = stringResource(R.string.usb_power_subtitle), trailing = { if (m.diskStopped) Chip(stringResource(R.string.usb_power_stopped_chip), severity = Severity.Warning) else if (m.diskIdleStopped) Chip(stringResource(R.string.usb_idle_stopped_chip), severity = Severity.Warning) }) {
                if (m.diskStopped) {
                    Text(stringResource(R.string.usb_power_stopped_desc), style = MaterialTheme.typography.bodyMedium)
                    FilledTonalButton(onClick = a::resumeDisk, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_power_resume)) }
                } else {
                    Text(stringResource(R.string.usb_power_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    OutlinedButton(onClick = a::stopDisk, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_power_stop)) }
                    Text(stringResource(R.string.usb_idle_title), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(R.string.usb_idle_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        listOf(0, 5, 10, 30).forEach { min ->
                            FilterChip(
                                selected = m.idleSpinDownMin == min,
                                onClick = { a.setIdleSpinDown(min) },
                                label = { Text(if (min == 0) stringResource(R.string.usb_idle_off) else stringResource(R.string.usb_idle_minutes, min)) },
                            )
                        }
                    }
                }
            }
        }

        // ---- パーティション
        val t = m.table
        if (isOpen && t != null) {
            PartitionList(t, m.usbLabel, canPublish = pub == null && m.busy == null && !m.diskStopped, writable = m.usbWritable) { p -> a.publishPartition(p.startBytes, p.sizeBytes, "${m.usbLabel} #${p.index}") }
            if (t.wholeDeviceVolume && pub == null) OutlinedButton(onClick = { a.publishPartition(0, t.diskBytes, m.usbLabel) }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_publish_whole_device)) }
            if (pub == null) OutlinedButton(onClick = { if (m.usbWritable) confirmClose = true else a.closeUsb() }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_close_without_publish)) }
        }

        // ---- 手順 (未接続のときだけ)
        if (!isOpen) {
            SectionCard(stringResource(R.string.usb_guide_title)) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Step(1, stringResource(R.string.usb_guide_1_title), stringResource(R.string.usb_guide_1_body), done = m.osMountedUsb == 0 && m.usbDevices.isNotEmpty())
                    Step(2, stringResource(R.string.usb_guide_2_title), stringResource(R.string.usb_guide_2_body))
                    Step(3, stringResource(R.string.usb_guide_3_title), stringResource(R.string.usb_guide_3_body))
                    Step(4, stringResource(R.string.usb_guide_4_title), stringResource(R.string.usb_guide_4_body))
                    Step(5, stringResource(R.string.usb_guide_5_title), stringResource(R.string.usb_guide_5_body))
                }
            }
        }
    }
    if (confirmClose) {
        ConfirmDialog(
            title = stringResource(R.string.usb_close_dialog_title),
            text = stringResource(R.string.usb_close_dialog_text),
            confirmLabel = stringResource(R.string.usb_close_dialog_confirm),
            onConfirm = { confirmClose = false; a.closeUsb() },
            onDismiss = { confirmClose = false },
        )
    }
}

@Composable
private fun PartitionList(t: PartitionTable, label: String, canPublish: Boolean, writable: Boolean, onPublish: (org.usbmem.app.Partition) -> Unit) {
    SectionCard(stringResource(R.string.common_partitions_title), subtitle = stringResource(R.string.common_partitions_subtitle, label, t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize)) {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            t.partitions.forEach { p ->
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                        Chip(p.fs.uppercase())
                    }
                    Text(stringResource(if (p.extendedContainer) R.string.common_partition_detail_extended else R.string.common_partition_detail, formatBytes(p.sizeBytes), p.typeName, p.startLba), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (p.fs == "ntfs" || p.fs == "exfat" || p.fs == "ext2") {
                        // exFAT / ext2 は読み取り専用 (書き込みは未対応)。NTFS だけ、書き込み許可のスイッチに従う
                        val canWrite = writable && p.fs == "ntfs"
                        Button(onClick = { onPublish(p) }, enabled = canPublish, modifier = Modifier.fillMaxWidth()) { Text(stringResource(if (canWrite) R.string.usb_publish_write else R.string.usb_publish_read_only)) }
                    }
                }
            }
            t.diagnostics.forEach { d -> Text("${d.severity}: ${d.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp)) }
        }
    }
}
