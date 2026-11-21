package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.NtfsEntry
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes

/** ネスト (ループバック) マウントの画面の操作 (実処理は MainActivity。IO スレッドから呼ばれる)。docs/nested-mount.md */
interface NestedActions {
    /** マウント済みのボリューム (イメージの中を探せる候補) */
    fun nestedVolumes(): List<NestedVolumeRow>
    /** ボリューム内のフォルダの一覧 (フォルダが先、名前順)。失敗は空 */
    fun nestedList(volumeId: String, path: String): List<NtfsEntry>
    /** イメージのパーティション表を読む (何もマウントしない) */
    fun nestedInspect(volumeId: String, path: String): NestedInspectResult
    /** 個人版: WIM を読み取り専用で公開する (別プロセスの wimlib)。image = イメージ番号 (1 始まり)、null = イメージごとのフォルダを並べて全部 */
    fun nestedMountWim(volumeId: String, path: String, image: Int?, label: String): NestedMountResult
    /** イメージの [startBytes, +sizeBytes) を読み取り専用でマウントして公開する。ext3/ext4 のジャーナル未再生は、確認後に allowNoload = true で呼ぶ */
    fun nestedMount(volumeId: String, path: String, startBytes: Long, sizeBytes: Long, label: String, allowNoload: Boolean): NestedMountResult
}

/** depth = そのボリュームのネスト段数 (最上位 = 0)。[canNest] = その中のイメージをさらに開ける (段数の上限未満) */
data class NestedVolumeRow(val id: String, val label: String, val fsName: String, val depth: Int, val canNest: Boolean)
data class NestedInspectResult(val table: PartitionTable? = null, val notice: Notice? = null, val wim: org.usbmem.app.wimfuse.WimInfo? = null)
/** [noload] = ジャーナル未再生 (EUCLEAN) で拒否された。確認すれば読み取り専用で開き直せる */
data class NestedMountResult(val notice: Notice, val noload: Boolean = false)

private val MOUNTABLE_FS = setOf("ntfs", "exfat", "ext2", "ext3", "ext4", "fat12", "fat16", "fat32", "iso9660", "udf", "squashfs", "erofs")

private data class NestedPick(val volumeId: String, val path: String, val table: PartitionTable?, val wim: org.usbmem.app.wimfuse.WimInfo? = null)
private data class NestedAsk(val volumeId: String, val path: String, val start: Long, val size: Long, val label: String)

@Composable
fun NestedMountSection(a: NestedActions, modifier: Modifier = Modifier) {
    val scope = rememberUiScope()
    var busy by remember { mutableStateOf(false) }
    var volumes by remember { mutableStateOf<List<NestedVolumeRow>?>(null) }   // null = 選び始めていない
    var volume by remember { mutableStateOf<NestedVolumeRow?>(null) }
    var dir by remember { mutableStateOf("/") }
    var entries by remember { mutableStateOf<List<NtfsEntry>>(emptyList()) }
    var pick by remember { mutableStateOf<NestedPick?>(null) }
    var notice by remember { mutableStateOf<Notice?>(null) }
    var ask by remember { mutableStateOf<NestedAsk?>(null) }

    fun <T> io(block: () -> T, then: (T) -> Unit) { scope.launch { busy = true; val r = withContext(Dispatchers.IO) { block() }; busy = false; then(r) } }
    fun browse(v: NestedVolumeRow, path: String) { io({ a.nestedList(v.id, path) }) { volume = v; dir = path; entries = it } }
    fun mountWim(p: NestedPick, image: Int?, label: String) {
        io({ a.nestedMountWim(p.volumeId, p.path, image, label) }) { r -> notice = r.notice }
    }
    fun mount(p: NestedPick, start: Long, size: Long, label: String, noload: Boolean) {
        io({ a.nestedMount(p.volumeId, p.path, start, size, label, noload) }) { r ->
            notice = r.notice
            if (r.noload) ask = NestedAsk(p.volumeId, p.path, start, size, label)
        }
    }

    SectionCard(stringResource(R.string.nested_title), modifier, subtitle = stringResource(R.string.nested_subtitle)) {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            val p = pick
            if (p != null) {
                // ---- 選んだイメージの中身 (パーティション) → マウント
                Text(p.path.substringAfterLast('/'), style = MaterialTheme.typography.titleSmall)
                val w = p.wim
                val t = p.table
                if (w != null) {
                    // ---- WIM (Windows イメージ。個人版): イメージを選ぶ。別プロセスの wimlib が読み、一時コピーは作らない
                    Text(stringResource(R.string.nested_wim_summary, w.imageCount, w.compression, formatBytes(w.totalBytes)), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (w.imageCount > 1) Button(onClick = { mountWim(p, null, p.path.substringAfterLast('/')) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.nested_wim_mount_all)) }
                    w.images.forEach { im ->
                        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("#${im.index} ${im.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                                Chip("WIM")
                            }
                            Text(stringResource(R.string.nested_wim_image_detail, im.files, im.dirs, formatBytes(im.bytes)) + if (im.description.isNotBlank()) "\n${im.description}" else "", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            Button(onClick = { mountWim(p, im.index, "${p.path.substringAfterLast('/')} #${im.index}") }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_publish_read_only)) }
                        }
                    }
                } else if (t == null) {
                    Text(stringResource(R.string.nested_nothing_to_mount), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else if (t.wholeDeviceVolume) {
                    Button(onClick = { mount(p, 0, t.diskBytes, p.path.substringAfterLast('/'), false) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.nested_mount_whole)) }
                } else {
                    Text(stringResource(R.string.common_partitions_subtitle, p.path.substringAfterLast('/'), t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    t.partitions.forEach { part ->
                        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("#${part.index} ${part.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                                Chip(part.fs.uppercase())
                            }
                            Text(stringResource(if (part.extendedContainer) R.string.common_partition_detail_extended else R.string.common_partition_detail, formatBytes(part.sizeBytes), part.typeName, part.startLba), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            if (part.fs in MOUNTABLE_FS) {
                                Button(onClick = { mount(p, part.startBytes, part.sizeBytes, "${p.path.substringAfterLast('/')} #${part.index}", false) }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.usb_publish_read_only)) }
                            }
                        }
                    }
                    if (t.partitions.isEmpty()) Text(stringResource(R.string.nested_nothing_to_mount), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                OutlinedButton(onClick = { pick = null; notice = null }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.nested_choose_another)) }
            } else if (volumes == null) {
                OutlinedButton(onClick = { notice = null; io({ a.nestedVolumes() }) { volumes = it; volume = null } }, enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.nested_choose_image)) }
            } else if (volume == null) {
                // ---- ボリュームを選ぶ
                val vs = volumes.orEmpty()
                if (vs.isEmpty()) Text(stringResource(R.string.nested_no_volumes), style = MaterialTheme.typography.bodyMedium)
                vs.forEach { v ->
                    OutlinedButton(onClick = { browse(v, "/") }, enabled = !busy && v.canNest, modifier = Modifier.fillMaxWidth()) {
                        Text(if (v.canNest) "${v.label} (${v.fsName})" else stringResource(R.string.nested_too_deep_volume, v.label))
                    }
                }
                TextButton(onClick = { volumes = null }, enabled = !busy) { Text(stringResource(R.string.common_cancel)) }
            } else {
                // ---- フォルダをたどってイメージのファイルを選ぶ
                val v = volume!!
                Text("${v.label}: $dir", style = MaterialTheme.typography.bodyMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { if (dir == "/") { volume = null } else browse(v, dir.substringBeforeLast('/', "").ifEmpty { "/" }) }, enabled = !busy) { Text(stringResource(R.string.nested_up)) }
                    TextButton(onClick = { volumes = null; volume = null }, enabled = !busy) { Text(stringResource(R.string.common_cancel)) }
                }
                entries.forEach { e ->
                    val child = if (dir == "/") "/${e.name}" else "$dir/${e.name}"
                    OutlinedButton(
                        onClick = {
                            if (e.isDir) browse(v, child)
                            else io({ a.nestedInspect(v.id, child) }) { r ->
                                notice = r.notice
                                if (r.wim != null) pick = NestedPick(v.id, child, null, r.wim)
                                else if (r.table != null) pick = NestedPick(v.id, child, r.table)
                            }
                        },
                        enabled = !busy && !e.isLink,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(if (e.isDir) "${e.name}/" else "${e.name}  (${formatBytes(e.size)})") }
                }
                if (entries.isEmpty()) Text(stringResource(R.string.nested_empty_folder), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            notice?.let { Banner(it) }
        }
    }
    ask?.let { q ->
        ConfirmDialog(
            title = stringResource(R.string.noload_dialog_title),
            text = noloadDialogText(fsNameOfRange(pick?.table, q.start, q.size)),
            confirmLabel = stringResource(R.string.noload_dialog_confirm),
            onConfirm = { ask = null; pick?.let { mount(it, q.start, q.size, q.label, true) } },
            onDismiss = { ask = null },
            subject = q.label,
        )
    }
}
