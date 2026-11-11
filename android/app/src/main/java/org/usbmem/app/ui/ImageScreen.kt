package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.PartitionTable
import org.usbmem.app.formatBytes

/** ディスクイメージ画面の結果。table = パーティション一覧、notice = 結果の通知。 */
data class ImageResult(val label: String? = null, val table: PartitionTable? = null, val notice: Notice? = null)

/** ディスクイメージ画面の操作 (実処理は MainActivity。呼び出しは IO スレッドで行われる)。 */
interface ImageActions {
    fun loadImage(uri: Uri): ImageResult
    fun shareImage(uri: Uri, writable: Boolean): ImageResult
}

@Composable
fun ImageScreen(a: ImageActions, modifier: Modifier = Modifier) {
    val scope = rememberCoroutineScope()
    var result by remember { mutableStateOf(ImageResult()) }
    var busy by remember { mutableStateOf(false) }
    var allowWrite by remember { mutableStateOf(false) }   // 書き込みは毎回の起動で明示的に許可する (保存しない)
    val openPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.loadImage(uri) }; busy = false }
    }
    val sharePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.shareImage(uri, allowWrite) }; busy = false }
    }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        SectionCard("パーティションを調べる", subtitle = "ディスクイメージ (.img / .bin) を読み取り専用で開きます。書き込みはしません") {
            Button(onClick = { openPicker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text("RAW イメージを開く") }
        }
        SectionCard(
            "NTFS イメージを公開",
            subtitle = "ファイルアプリから中身を見られるようにします",
            trailing = { Chip(if (allowWrite) "書き込み可 (実験的)" else "読み取り専用", severity = if (allowWrite) Severity.Warning else Severity.Success) },
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("書き込みを許可 (実験的)", style = MaterialTheme.typography.bodyLarge)
                    Text("データ破損の恐れがあります。バックアップを取ってください。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = allowWrite, onCheckedChange = { allowWrite = it }, enabled = !busy)
            }
            OutlinedButton(onClick = { sharePicker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) {
                Text(if (allowWrite) "NTFS イメージを公開 (書き込み可)" else "NTFS イメージを公開 (読み取り専用)")
            }
        }
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        result.notice?.let { Banner(it) }
        result.table?.let { t ->
            SectionCard("パーティション", subtitle = "${result.label ?: "image"} · ${t.kind.uppercase()} · ${formatBytes(t.diskBytes)} · セクタ ${t.sectorSize}B") {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    t.partitions.forEach { p ->
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                                Chip(p.fs.uppercase())
                            }
                            Text("${formatBytes(p.sizeBytes)} · ${p.typeName} · 開始 LBA ${p.startLba}" + if (p.extendedContainer) " · 拡張枠" else "", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    t.diagnostics.forEach { d -> Text("${d.severity}: ${d.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
                }
            }
        }
    }
}
