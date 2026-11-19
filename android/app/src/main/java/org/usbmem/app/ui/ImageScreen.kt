package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
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
import androidx.compose.runtime.saveable.rememberSaveable
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
    /** 非 null = ダーティフラグだけの NTFS に書き込み可を要求して、確認待ち (何も開いていない)。確認のダイアログを出す */
    val dirtyUri: Uri? = null,
    /** 確認ダイアログに出す、そのイメージのファイル名 ([dirtyUri] / [noloadUri] の分) */
    val dirtyName: String? = null, val noloadName: String? = null,
    /** [noloadUri] のイメージの FS 名 ("ext4" など。分からなければ null): 確認の文言を、その FS のものだけにする */
    val noloadFs: String? = null,
    /** 調べたイメージの URI (暗号ボリュームの解錠で、もう一度開くのに使う)。イメージ内のイメージ (ネスト) は null */
    val uri: Uri? = null,
)

/** ディスクイメージ画面の操作 (実処理は MainActivity。呼び出しは IO スレッドで行われる)。 */
interface ImageActions : NestedActions {
    fun loadImage(uri: Uri): ImageResult
    fun shareImage(uri: Uri, writable: Boolean): ImageResult
    /** ext3/ext4 のジャーナル未再生を承知のうえ (確認後) で、読み取り専用で開く。 */
    fun shareImageNoload(uri: Uri): ImageResult
    /** ダーティフラグだけの NTFS を、確認のうえ書き込み可で開く (読み取り専用で開くほうは [shareImage] の writable = false)。 */
    fun shareImageDirty(uri: Uri): ImageResult
    /** イメージの [startBytes, +sizeBytes) が暗号ボリュームなら、解錠ダイアログの内容を返す (資格情報は使わない。何もマウントしない)。 */
    fun probeCrypto(uri: Uri, startBytes: Long, sizeBytes: Long, label: String): CryptoProbeResult
    /** 解錠して、中のファイルシステムをマウント・公開する。[UnlockInput.cred] は呼び出しの最後に消す。 */
    fun unlockImage(uri: Uri, startBytes: Long, sizeBytes: Long, label: String, input: UnlockInput): CryptoUnlockResult
    /** 解錠したが中のジャーナルが未再生で、「読み取り専用で開く」確認を待っているイメージ由来のボリューム (画面が作り直されたあとに、ダイアログを出し直すのに使う) */
    fun pendingCryptoNoload(): List<CryptoNoloadPrompt>
    /** 解錠ダイアログの「記憶を削除」: [volumeId] のパスワードの記憶を消す (IO スレッドから呼ばれる)。 */
    fun forgetRemembered(volumeId: String)
    /** 確認のうえ、読み取り専用 (noload) で開く。[writeDirty] = true なら、ダーティフラグだけの NTFS は書き込み可で開く。 */
    fun confirmCryptoNoload(id: String, writeDirty: Boolean = false): CryptoUnlockResult
    /** キャンセル: 保留していた復号済みデバイスを閉じる (= ロック。鍵を消す)。 */
    fun cancelCryptoNoload(id: String)
    /** 解錠済みの暗号ボリューム (USB から解錠したものも含む) */
    fun cryptoVolumes(): List<CryptoVolumeRow>
    /** ロック: 中のファイルシステムをアンマウントして、鍵を消す。 */
    fun lockCrypto(id: String)
    /** 公開中の (暗号ではない) イメージのボリューム。USB 由来・ネストした子・暗号ボリュームの中身は含まない。 */
    fun imageVolumes(): List<ImageVolumeRow>
    /** 閉じる: クリーンにアンマウントして (書き込みの確定・ダーティフラグを下ろす)、再起動後の復元の一覧からも外す。成功した通知を返す。 */
    fun closeImage(id: String): Notice
}

/** 公開中のイメージのボリューム 1 つ。[id] = VolumeRegistry の id (閉じる操作に渡す)。 */
data class ImageVolumeRow(val id: String, val label: String, val fsName: String, val writable: Boolean)

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ImageScreen(a: ImageActions, modifier: Modifier = Modifier) {
    val scope = rememberUiScope()
    val appCtx = LocalContext.current
    var result by remember { mutableStateOf(ImageResult()) }
    var busy by remember { mutableStateOf(false) }
    // ダーティフラグだけの NTFS に書き込み可を要求して、確認待ちのイメージ。何も保持しない (開き直すだけ) ので、画面の作り直し (回転) でも失わないよう保存する
    var dirtyUri by rememberSaveable { mutableStateOf<Uri?>(null) }
    var dirtyName by rememberSaveable { mutableStateOf<String?>(null) }
    fun applyShare(r: ImageResult) { result = r; dirtyUri = r.dirtyUri; dirtyName = r.dirtyName }
    var allowWrite by remember { mutableStateOf(false) }   // 書き込みは毎回の起動で明示的に許可する (保存しない)
    val openPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.loadImage(uri) }; busy = false }
    }
    var unlockTarget by remember { mutableStateOf<Triple<Uri, String, UnlockTarget>?>(null) }
    var unlocking by remember { mutableStateOf(false) }
    var cryptoRows by remember { mutableStateOf<List<CryptoVolumeRow>>(emptyList()) }
    var imageRows by remember { mutableStateOf<List<ImageVolumeRow>>(emptyList()) }
    fun refreshCrypto() { scope.launch { cryptoRows = withContext(Dispatchers.IO) { a.cryptoVolumes() } } }
    var cryptoNoload by remember { mutableStateOf<CryptoNoloadPrompt?>(null) }   // 保留は VolumeRegistry が持つ。画面が作り直されても、下の LaunchedEffect で出し直す
    LaunchedEffectUi(Unit) {
        cryptoRows = withContext(Dispatchers.IO) { a.cryptoVolumes() }
        cryptoNoload = withContext(Dispatchers.IO) { a.pendingCryptoNoload() }.firstOrNull()
    }
    // 公開中のイメージの一覧は、結果が変わるたび (共有・閉じる・解錠) に取り直す
    LaunchedEffectUi(result, busy) { if (!busy) imageRows = withContext(Dispatchers.IO) { a.imageVolumes() } }
    fun askUnlock(uri: Uri, p: org.usbmem.app.Partition, label: String) {
        scope.launch {
            busy = true
            val r = withContext(Dispatchers.IO) { a.probeCrypto(uri, p.startBytes, p.sizeBytes, label) }
            busy = false
            if (r.prompt != null) unlockTarget = Triple(uri, label, UnlockTarget(p.startBytes, p.sizeBytes, r.prompt)) else result = result.copy(notice = r.notice)
        }
    }
    val sharePicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) scope.launch { busy = true; applyShare(withContext(Dispatchers.IO) { a.shareImage(uri, allowWrite) }); busy = false }
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
                Switch(checked = allowWrite, onCheckedChange = { allowWrite = it }, enabled = !busy, modifier = Modifier.testTag("image_write_switch"))
            }
            OutlinedButton(onClick = { sharePicker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth().testTag("image_share_button")) {
                Text(stringResource(if (allowWrite) R.string.image_share_write else R.string.image_share_read_only))
            }
        }
        if (imageRows.isNotEmpty()) {
            SectionCard(stringResource(R.string.image_mounted_title), subtitle = stringResource(R.string.image_mounted_subtitle)) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    imageRows.forEach { r ->
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Column(Modifier.weight(1f)) {
                                Text(r.label, style = MaterialTheme.typography.titleSmall)
                                Text(r.fsName.uppercase() + " · " + stringResource(if (r.writable) R.string.image_mounted_mode_write else R.string.common_mode_read_only), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            OutlinedButton(
                                onClick = { scope.launch { busy = true; val n = withContext(Dispatchers.IO) { a.closeImage(r.id) }; busy = false; result = result.copy(notice = n) } },
                                enabled = !busy, modifier = Modifier.testTag("image_close"),
                            ) { Text(stringResource(R.string.image_close_button)) }
                        }
                    }
                }
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
                text = noloadDialogText(result.noloadFs),
                confirmLabel = stringResource(R.string.noload_dialog_confirm),
                onConfirm = { scope.launch { busy = true; result = withContext(Dispatchers.IO) { a.shareImageNoload(u) }; busy = false } },
                onDismiss = { result = result.copy(noloadUri = null) },
                subject = result.noloadName,
            )
        }
        dirtyUri?.let { u ->
            DirtyWriteDialog(
                text = stringResource(R.string.dirty_dialog_text),
                onOpenReadOnly = { dirtyUri = null; scope.launch { busy = true; applyShare(withContext(Dispatchers.IO) { a.shareImage(u, false) }); busy = false } },
                onOpenWrite = { dirtyUri = null; scope.launch { busy = true; applyShare(withContext(Dispatchers.IO) { a.shareImageDirty(u) }); busy = false } },
                onCancel = { dirtyUri = null; result = result.copy(notice = Notice(Severity.Info, appCtx.getString(R.string.dirty_cancelled))) },
                subject = dirtyName,
            )
        }
        cryptoNoload?.let { p ->
            fun confirm(writeDirty: Boolean) {
                cryptoNoload = null
                scope.launch {
                    busy = true
                    val r = withContext(Dispatchers.IO) { a.confirmCryptoNoload(p.id, writeDirty) }
                    busy = false
                    result = result.copy(notice = r.notice); refreshCrypto()
                }
            }
            CryptoPendingDialog(
                p,
                onOpenReadOnly = { confirm(false) },
                onOpenWrite = { confirm(true) },
                onCancel = {
                    cryptoNoload = null
                    scope.launch {
                        busy = true; withContext(Dispatchers.IO) { a.cancelCryptoNoload(p.id) }; busy = false
                        result = result.copy(notice = Notice(Severity.Info, appCtx.getString(R.string.crypto_noload_cancelled)))
                    }
                },
            )
        }
        unlockTarget?.let { (uri, label, t) ->
            UnlockDialog(
                t.prompt, writeAvailable = true, writeInitial = false, busy = unlocking,
                onCancel = { unlockTarget = null },
                onForget = {
                    scope.launch {
                        withContext(Dispatchers.IO) { a.forgetRemembered(t.prompt.volumeId) }
                        unlockTarget = unlockTarget?.let { (u, l, tt) -> Triple(u, l, tt.copy(prompt = tt.prompt.copy(remembered = false))) }
                    }
                },
                onSubmit = { input ->
                    scope.launch {
                        unlocking = true
                        val r = withContext(Dispatchers.IO) { a.unlockImage(uri, t.startBytes, t.sizeBytes, label, input) }
                        unlocking = false
                        if (r.retryError != null) unlockTarget = Triple(uri, label, t.copy(prompt = t.prompt.copy(error = r.retryError, remembered = r.rememberedNow ?: t.prompt.remembered)))
                        else { unlockTarget = null; result = result.copy(notice = r.notice); cryptoNoload = r.noload; refreshCrypto() }
                    }
                },
            )
        }
        // 結果の通知はこの画面の下のほうに出るので、出たら見える所までスクロールする (長い警告を見落とさないように)
        result.notice?.let { Banner(it, Modifier.testTag("image_notice"), scrollIntoView = true) }
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
