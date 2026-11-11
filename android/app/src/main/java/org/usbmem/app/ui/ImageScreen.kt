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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
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
        SectionCard(stringResource(R.string.image_inspect_title), subtitle = stringResource(R.string.image_inspect_subtitle)) {
            Button(onClick = { openPicker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.image_open_raw)) }
        }
        SectionCard(
            stringResource(R.string.image_share_title),
            subtitle = stringResource(R.string.image_share_subtitle),
            trailing = { Chip(stringResource(if (allowWrite) R.string.image_chip_write else R.string.common_mode_read_only), severity = if (allowWrite) Severity.Warning else Severity.Success) },
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(R.string.common_write_allow), style = MaterialTheme.typography.bodyLarge)
                    Text(stringResource(R.string.image_write_warning), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = allowWrite, onCheckedChange = { allowWrite = it }, enabled = !busy)
            }
            OutlinedButton(onClick = { sharePicker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(if (allowWrite) R.string.image_share_write else R.string.image_share_read_only))
            }
        }
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        result.notice?.let { Banner(it) }
        result.table?.let { t ->
            SectionCard(stringResource(R.string.common_partitions_title), subtitle = stringResource(R.string.common_partitions_subtitle, result.label ?: "image", t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize)) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    t.partitions.forEach { p ->
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                                Chip(p.fs.uppercase())
                            }
                            Text(stringResource(if (p.extendedContainer) R.string.common_partition_detail_extended else R.string.common_partition_detail, formatBytes(p.sizeBytes), p.typeName, p.startLba), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    t.diagnostics.forEach { d -> Text("${d.severity}: ${d.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
                }
            }
        }
    }
}
