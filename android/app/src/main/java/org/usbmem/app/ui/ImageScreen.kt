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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.CryptoMount
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes

/** ディスクイメージ画面の結果。table = パーティション一覧、notice = 結果の通知。 */
data class ImageResult(
    val label: String? = null, val table: PartitionTable? = null, val notice: Notice? = null, val noloadUri: Uri? = null,
    /** 調べたイメージの URI (暗号ボリュームの解錠で、もう一度開くのに使う)。イメージ内のイメージ (ネスト) は null */
    val uri: Uri? = null,
)

/** ディスクイメージ画面の操作 (実処理は MainActivity。呼び出しは IO スレッドで行われる)。 */
interface ImageActions : NestedActions {
    fun loadImage(uri: Uri): ImageResult
    fun shareImage(uri: Uri, writable: Boolean): ImageResult
    /** ext3/ext4 のジャーナル未再生を承知のうえ (確認後) で、読み取り専用で開く。 */
    fun shareImageNoload(uri: Uri): ImageResult
    /** イメージの [startBytes, +sizeBytes) が暗号ボリュームなら、解錠ダイアログの内容を返す (資格情報は使わない。何もマウントしない)。 */
    fun probeCrypto(uri: Uri, startBytes: Long, sizeBytes: Long, label: String): CryptoProbeResult
    /** 解錠して、中のファイルシステムをマウント・公開する。[UnlockInput.cred] は呼び出しの最後に消す。 */
    fun unlockImage(uri: Uri, startBytes: Long, sizeBytes: Long, label: String, input: UnlockInput): CryptoUnlockResult
    /** 解錠済みの暗号ボリューム (USB から解錠したものも含む) */
    fun cryptoVolumes(): List<CryptoVolumeRow>
    /** ロック: 中のファイルシステムをアンマウントして、鍵を消す。 */
    fun lockCrypto(id: String)
}

@Composable
fun ImageScreen(a: ImageActions, modifier: Modifier = Modifier) {
    val scope = rememberCoroutineScope()
    val appCtx = LocalContext.current
    var result by remember { mutableStateOf(ImageResult()) }
    var busy by remember { mutableStateOf(false) }
    var allowWrite by remember { mutableStateOf(false) }   // 書き込みは毎回の起動で明示的に許可する (保存しない)
    val openPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.loadImage(uri) }; busy = false }
    }
    var unlockTarget by remember { mutableStateOf<Triple<Uri, String, UnlockTarget>?>(null) }
    var unlocking by remember { mutableStateOf(false) }
    var cryptoRows by remember { mutableStateOf<List<CryptoVolumeRow>>(emptyList()) }
    fun refreshCrypto() { scope.launch { cryptoRows = withContext(Dispatchers.IO) { a.cryptoVolumes() } } }
    LaunchedEffect(Unit) { cryptoRows = withContext(Dispatchers.IO) { a.cryptoVolumes() } }
    fun askUnlock(uri: Uri, p: org.usbmem.app.Partition, label: String) {
        scope.launch {
            busy = true
            val r = withContext(Dispatchers.IO) { a.probeCrypto(uri, p.startBytes, p.sizeBytes, label) }
            busy = false
            if (r.prompt != null) unlockTarget = Triple(uri, label, UnlockTarget(p.startBytes, p.sizeBytes, r.prompt)) else result = result.copy(notice = r.notice)
        }
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
        if (cryptoRows.isNotEmpty()) {
            SectionCard(stringResource(R.string.crypto_section_title), subtitle = stringResource(R.string.crypto_section_subtitle)) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    cryptoRows.forEach { r ->
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Column(Modifier.weight(1f)) {
                                Text(r.label, style = MaterialTheme.typography.titleSmall)
                                Text("${r.format} · ${r.fsName.uppercase()} · " + stringResource(if (r.writable) R.string.crypto_mode_encrypted_write else R.string.crypto_mode_encrypted_read_only), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            OutlinedButton(onClick = { scope.launch { busy = true; withContext(Dispatchers.IO) { a.lockCrypto(r.id) }; busy = false; refreshCrypto(); result = result.copy(notice = Notice(Severity.Info, appCtx.getString(R.string.crypto_locked))) } }, enabled = !busy, modifier = Modifier.testTag("crypto_lock")) { Text(stringResource(R.string.crypto_lock_button)) }
                        }
                    }
                }
            }
        }
        NestedMountSection(a)   // マウント済みのボリュームの中のイメージを、読み取り専用でマウント (ネスト / ループバック)
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        result.noloadUri?.let { u ->
            ConfirmDialog(
                title = stringResource(R.string.noload_dialog_title),
                text = stringResource(R.string.noload_dialog_text),
                confirmLabel = stringResource(R.string.noload_dialog_confirm),
                onConfirm = { scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.shareImageNoload(u) }; busy = false } },
                onDismiss = { result = result.copy(noloadUri = null) },
            )
        }
        unlockTarget?.let { (uri, label, t) ->
            UnlockDialog(
                t.prompt, writeAvailable = true, writeInitial = false, busy = unlocking,
                onCancel = { unlockTarget = null },
                onSubmit = { input ->
                    scope.launch {
                        unlocking = true
                        val r = withContext(Dispatchers.IO) { a.unlockImage(uri, t.startBytes, t.sizeBytes, label, input) }
                        unlocking = false
                        if (r.retryError != null) unlockTarget = Triple(uri, label, t.copy(prompt = t.prompt.copy(error = r.retryError)))
                        else { unlockTarget = null; result = result.copy(notice = r.notice); refreshCrypto() }
                    }
                },
            )
        }
        result.notice?.let { Banner(it, Modifier.testTag("image_notice")) }
        result.table?.let { t ->
            SectionCard(stringResource(R.string.common_partitions_title), subtitle = stringResource(R.string.common_partitions_subtitle, result.label ?: "image", t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize)) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    t.partitions.forEach { p ->
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                                Chip(p.fs.uppercase())
                                if (CryptoMount.isCryptoFs(p.fs)) Chip(stringResource(R.string.crypto_chip_encrypted))
                            }
                            Text(stringResource(if (p.extendedContainer) R.string.common_partition_detail_extended else R.string.common_partition_detail, formatBytes(p.sizeBytes), p.typeName, p.startLba), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            val src = result.uri
                            if (CryptoMount.isCryptoFs(p.fs) && src != null) {
                                val lbl = (result.label ?: "image") + (if (t.wholeDeviceVolume) "" else " #${p.index}")
                                Button(onClick = { askUnlock(src, p, lbl) }, enabled = !busy, modifier = Modifier.fillMaxWidth().testTag("crypto_unlock_button")) { Text(stringResource(R.string.crypto_unlock_button)) }
                            }
                        }
                    }
                    t.diagnostics.forEach { d -> Text("${d.severity}: ${d.message}", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
                }
            }
        }
    }
}
