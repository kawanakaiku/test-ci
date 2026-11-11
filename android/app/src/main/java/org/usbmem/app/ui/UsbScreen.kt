package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.usbmem.app.PartitionTable
import org.usbmem.app.formatBytes

/** USB 画面で起こせる操作。実処理は MainActivity。 */
interface UsbActions {
    fun openStorageSettings()
    fun openUsb(key: String)
    fun publishPartition(startBytes: Long, sizeBytes: Long, label: String)
    fun closeUsb()
    fun setAllowWrite(v: Boolean)
}

@Composable
fun UsbScreen(m: AppModel, a: UsbActions, modifier: Modifier = Modifier) {
    var confirmClose by remember { mutableStateOf(false) }
    val isOpen = m.usbOpenKey != null
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ---- モード (USB を開く前に決める)
        SectionCard(
            "書き込みモード",
            subtitle = "USB を開く前に決めます。開いている間は変えられません",
            trailing = { Chip(if (m.allowUsbWrite) "新規作成のみ" else "読み取り専用", severity = if (m.allowUsbWrite) Severity.Warning else Severity.Success) },
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("書き込みを許可 (実験的)", style = MaterialTheme.typography.bodyLarge)
                    Text("新しいフォルダとファイルの作成だけができます。既存のファイルの変更・削除・名前変更はできません。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = m.allowUsbWrite, onCheckedChange = { a.setAllowWrite(it) }, enabled = !isOpen && m.busy == null)
            }
        }

        // ---- 接続前の案内 / OS のマウント警告
        if (!isOpen && m.osMountedUsb > 0) {
            Banner(
                Notice(Severity.Warning, "OS が USB ボリュームをマウント中です (${m.osMountedUsb} 個)", "このまま開くと、OS のドライバを強制的に外すことになります。先に取り出してください。"),
                action = { OutlinedButton(onClick = a::openStorageSettings) { Text("ストレージ設定を開く") } },
            )
        }

        // ---- デバイス
        if (m.usbDevices.isEmpty()) {
            Banner(Notice(Severity.Info, "対応する USB マスストレージが見つかりません", "USB ストレージ (Bulk-Only) を接続してください。"))
        } else {
            m.usbDevices.forEach { d ->
                val open = m.usbOpenKey == d.key
                SectionCard(d.title, subtitle = d.vidPid, trailing = { if (open) Chip("接続中", severity = Severity.Success) }) {
                    if (!open) {
                        Button(onClick = { a.openUsb(d.key) }, enabled = m.busy == null && !isOpen, modifier = Modifier.fillMaxWidth()) {
                            Text(if (m.allowUsbWrite) "開く (新規作成のみ書き込み可)" else "開く (読み取り専用)")
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
            SectionCard("公開中", subtitle = pub.label, trailing = { Chip(pub.modeText, severity = if (pub.writable) Severity.Warning else Severity.Success) }) {
                Text("ファイルアプリの一覧に「USB Storage Manager」として表示されます。", style = MaterialTheme.typography.bodyMedium)
                if (pub.writable) Text("作業が終わったら、必ず下のボタンで閉じてください。アプリを強制終了したり USB を抜くと、NTFS がダーティのまま残り、Windows で chkdsk が走ります。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { if (m.usbWritable) confirmClose = true else a.closeUsb() }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text("USB を安全に閉じる") }
            }
        }

        // ---- パーティション
        val t = m.table
        if (isOpen && t != null) {
            PartitionList(t, m.usbLabel, canPublish = pub == null && m.busy == null, writable = m.usbWritable) { p -> a.publishPartition(p.startBytes, p.sizeBytes, "${m.usbLabel} #${p.index}") }
            if (t.wholeDeviceVolume && pub == null) OutlinedButton(onClick = { a.publishPartition(0, t.diskBytes, m.usbLabel) }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text("デバイス全体を公開 (読み取り専用)") }
            if (pub == null) OutlinedButton(onClick = { if (m.usbWritable) confirmClose = true else a.closeUsb() }, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text("公開せずに USB を閉じる") }
        }

        // ---- 手順 (未接続のときだけ)
        if (!isOpen) {
            SectionCard("使い方") {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Step(1, "OS が自動でマウントしたボリュームを取り出す", "多くの USB ディスクは、接続すると小さな FAT 領域 (EFI など) を OS がマウントします。ストレージ設定から取り出します。", done = m.osMountedUsb == 0 && m.usbDevices.isNotEmpty())
                    Step(2, "書き込みモードを選ぶ", "迷ったら読み取り専用のままにします。")
                    Step(3, "USB を開く", "初回は USB の許可ダイアログが出ます。")
                    Step(4, "NTFS パーティションを公開する", "公開すると、ファイルアプリから見られます。")
                    Step(5, "終わったら「USB を安全に閉じる」", "書き込みの確定とアンマウントをします。その後で取り外せます。")
                }
            }
        }
    }
    if (confirmClose) {
        ConfirmDialog(
            title = "USB を閉じますか？",
            text = "書き込みを確定して NTFS をアンマウントします。終わるまで数秒かかることがあります。閉じた後は、USB を取り外せます。",
            confirmLabel = "閉じる",
            onConfirm = { confirmClose = false; a.closeUsb() },
            onDismiss = { confirmClose = false },
        )
    }
}

@Composable
private fun PartitionList(t: PartitionTable, label: String, canPublish: Boolean, writable: Boolean, onPublish: (org.usbmem.app.Partition) -> Unit) {
    SectionCard("パーティション", subtitle = "$label · ${t.kind.uppercase()} · ${formatBytes(t.diskBytes)} · セクタ ${t.sectorSize}B") {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            t.partitions.forEach { p ->
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                        Chip(p.fs.uppercase())
                    }
                    Text("${formatBytes(p.sizeBytes)} · ${p.typeName} · 開始 LBA ${p.startLba}" + if (p.extendedContainer) " · 拡張枠" else "", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (p.fs == "ntfs") {
                        Button(onClick = { onPublish(p) }, enabled = canPublish, modifier = Modifier.fillMaxWidth()) { Text(if (writable) "公開 (新規作成のみ書き込み可)" else "公開 (読み取り専用)") }
                    }
                }
            }
            t.diagnostics.forEach { d -> Text("${d.severity}: ${d.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp)) }
        }
    }
}
