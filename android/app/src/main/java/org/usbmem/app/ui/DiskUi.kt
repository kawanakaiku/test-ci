// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.content.Context
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateMap
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.usbmem.app.CryptoMount
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes

/*
 * ディスクツールの共通部品 (見出し・確認ダイアログ・範囲の選択・パーティション構成のグラフ・ジョブの進捗)。docs/disk-ui.md
 *
 * 見た目の約束: 読み取り専用の操作は **緑 (成功色) + 鍵のアイコン + 「読み取り専用」**、書き込む操作は **赤 (エラー色) + 警告のアイコン + 「書き込みあり」**。
 * 色だけに頼らない (アイコンと文字でも区別する)。破壊的な操作の実行ボタンは赤。
 */

/** ディスクツールの種類。 */
enum class DiskTool(val title: Int, val subtitle: Int, val icon: Int, val destructive: Boolean, val tag: String) {
    Check(R.string.dt_tool_check, R.string.dt_tool_check_sub, R.drawable.ic_dt_search, false, "disk_tool_check"),
    Format(R.string.dt_tool_format, R.string.dt_tool_format_sub, R.drawable.ic_dt_delete, true, "disk_tool_format"),
    Partition(R.string.dt_tool_partition, R.string.dt_tool_partition_sub, R.drawable.ic_tune, true, "disk_tool_partition"),
    Backup(R.string.dt_tool_backup, R.string.dt_tool_backup_sub, R.drawable.ic_dt_backup, true, "disk_tool_backup"),
    Rescue(R.string.dt_tool_rescue, R.string.dt_tool_rescue_sub, R.drawable.ic_dt_build, false, "disk_tool_rescue"),
}

/** ディスクツール画面の遷移 (プロセスで 1 つ)。USB 画面 / イメージ画面の入口が [open] し、[DiskToolsHost] が表示する。 */
object DiskToolsNav {
    var target by mutableStateOf<DiskTarget?>(null)
    var tool by mutableStateOf<DiskTool?>(null)
    fun open(t: DiskTarget, tool: DiskTool? = null) { target = t; this.tool = tool }
    fun close() { target = null; tool = null }
}

/** 画面がツールへ渡す共通の状態。 */
class ToolContext(
    val a: DiskToolsActions,
    val target: DiskTarget,
    val table: PartitionTable,
    /** 今、書き込める ([DiskToolsActions.writeAllowed]) */
    val writeAllowed: Boolean,
    /** パーティション表を読み直す (編集・フォーマットのあと) */
    val reload: () -> Unit,
    /** 別のツールへ (例: フォーマットのあと「検査で確認する」) */
    val openTool: (DiskTool, Int) -> Unit,
    /** 入ったとき / 切り替えたときに選んでおく範囲の番号 */
    val initialRange: Int,
)

// ================================================================ 機能問い合わせ (他の担当の画面を差し込む口)

/** パーティション編集の画面に出す「口」。API ができた担当が [DiskToolsFeatures.register] するまで、この口は画面に出ない (「準備中」は出さない)。 */
enum class DiskSlot(val title: Int, val subtitle: Int, val tag: String) {
    LostPartitions(R.string.dt_slot_lost_title, R.string.dt_slot_lost_sub, "disk_slot_lost_partitions"),
    BootSector(R.string.dt_slot_boot_title, R.string.dt_slot_boot_sub, "disk_slot_boot_sector"),
}

/**
 * 口に差し込む画面の本体 (パーティション探索 / ブートセクタ修復の担当が、API ができたときに登録する。登録のしかたは docs/disk-ui.md)。
 * 渡される [ToolContext] で、対象・パーティション表・書き込み可否・表の読み直しが使える。
 */
object DiskToolsFeatures {
    private val bodies: SnapshotStateMap<DiskSlot, @Composable (ToolContext) -> Unit> = mutableStateMapOf()
    fun register(slot: DiskSlot, body: @Composable (ToolContext) -> Unit) { bodies[slot] = body }
    fun unregister(slot: DiskSlot) { bodies.remove(slot) }
    /** その口の機能が使えるか (登録済みか)。口の外から機能の有無を問い合わせるのにも使う */
    fun available(slot: DiskSlot): Boolean = bodies.containsKey(slot)
    internal fun body(slot: DiskSlot): (@Composable (ToolContext) -> Unit)? = bodies[slot]
}

/** パーティション編集の画面に置く、機能の口。登録が無ければ何も出さない。 */
@Composable
fun DiskFeatureSlots(ctx: ToolContext) {
    DiskSlot.entries.forEach { slot ->
        val body = DiskToolsFeatures.body(slot) ?: return@forEach
        SectionCard(stringResource(slot.title), subtitle = stringResource(slot.subtitle), modifier = Modifier.testTag(slot.tag)) { body(ctx) }
    }
}

// ================================================================ 文言

/** ネイティブの機械可読な理由コード → 利用者に見せる文言。分からないコードはそのまま返す。 */
fun diskCodeLabel(ctx: Context, code: String): String {
    val id = when (code) {
        "in-use", "target-in-use" -> R.string.dt_code_in_use
        "device-read-only", "target-read-only", "output-read-only" -> R.string.dt_code_read_only
        "busy" -> R.string.dt_code_busy
        "bad-token" -> R.string.dt_code_bad_token
        "plan-tampered", "plan-changed", "plan-invalid" -> R.string.dt_code_plan_changed
        "device-changed" -> R.string.dt_code_device_changed
        "wrong-device" -> R.string.dt_code_wrong_device
        "backup-required", "backup-failed" -> R.string.dt_code_backup_failed
        "write-failed" -> R.string.dt_code_write_failed
        "verify-failed" -> R.string.dt_code_verify_failed
        "cancelled" -> R.string.dt_code_cancelled
        "force-required" -> R.string.dt_code_force_required
        "too-small", "target-too-small", "output-too-small" -> R.string.dt_code_too_small
        "too-large" -> R.string.dt_code_too_large
        "fs-not-clean" -> R.string.dt_code_fs_not_clean
        "same-device", "same-file" -> R.string.dt_code_same_device
        "bad-mapfile" -> R.string.dt_code_bad_mapfile
        "mapfile-unreadable" -> R.string.dt_code_mapfile_unreadable
        "overlap" -> R.string.dt_code_overlap
        "image-corrupt" -> R.string.dt_code_image_corrupt
        "encrypted" -> R.string.dt_code_encrypted
        "unsupported-fs" -> R.string.dt_code_unsupported_fs
        else -> return code
    }
    return ctx.getString(id)
}

@Composable
fun diskCode(code: String): String = diskCodeLabel(androidx.compose.ui.platform.LocalContext.current, code)

/** パーティションの FS 名を画面に出す形に (空 / unknown は「不明」)。 */
@Composable
fun fsText(fs: String): String = if (fs.isBlank() || fs == "unknown") stringResource(R.string.dt_fs_unknown) else fs.uppercase()

// ================================================================ 範囲の一覧

/** ディスク全体 + パーティション。パーティション表のないイメージ (全体が 1 つの FS) は全体だけ。 */
fun rangesOf(t: PartitionTable, wholeLabel: String): List<DiskRange> {
    val first = t.partitions.firstOrNull()
    val whole = DiskRange(0, t.diskBytes, wholeLabel, if (t.wholeDeviceVolume) first?.fs ?: "" else "", 0, first?.takeIf { t.wholeDeviceVolume }?.fsLabel ?: "", first?.takeIf { t.wholeDeviceVolume }?.fsUuid ?: "")
    if (t.wholeDeviceVolume) return listOf(whole)
    val ps = t.partitions.filter { !it.extendedContainer && !it.protective && it.sizeBytes > 0 }.map {
        DiskRange(it.startBytes, it.sizeBytes, "#${it.index} ${it.name}".trim(), it.fs, it.index, it.fsLabel, it.fsUuid)
    }
    return listOf(whole) + ps
}

fun initialRangeIndex(ranges: List<DiskRange>, preselectStart: Long, preferred: Int): Int {
    if (preferred >= 0 && ranges.any { it.index == preferred }) return preferred
    if (preselectStart >= 0) ranges.firstOrNull { !it.whole && it.start == preselectStart }?.let { return it.index }
    return 0
}

/** 範囲を 1 つ選ぶ (ラジオボタン)。[enabled] が false の行は選べない。[note] = 行の下に出す一言 (null なら出さない)。 */
@Composable
fun RangePicker(ranges: List<DiskRange>, selected: Int, onSelect: (DiskRange) -> Unit, enabled: (DiskRange) -> Boolean = { true }, note: @Composable (DiskRange) -> String? = { null }) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        ranges.forEach { r ->
            val on = enabled(r)
            val sel = r.index == selected
            Row(
                Modifier.fillMaxWidth().selectable(selected = sel, enabled = on, role = Role.RadioButton, onClick = { onSelect(r) }).testTag("range_${r.index}").padding(vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                RadioButton(selected = sel, onClick = null, enabled = on)
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(r.label, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                        if (r.fs.isNotBlank()) Chip(fsText(r.fs))
                        if (CryptoMount.isCryptoFs(r.fs)) Chip(stringResource(R.string.crypto_chip_encrypted))
                    }
                    val sub = buildString {
                        append(formatBytes(r.size))
                        if (!r.whole) append(" · LBA ").append(r.start)
                        if (r.fsLabel.isNotBlank()) append(" · ").append(r.fsLabel)
                    }
                    Text(sub, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    note(r)?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error) }
                }
            }
        }
    }
}

// ================================================================ 見出し (読み取り専用 / 書き込みあり)

@Composable
fun ToolHeader(destructive: Boolean, title: String, text: String, modifier: Modifier = Modifier, tag: String? = null) {
    val st = LocalStatusColors.current
    val cs = MaterialTheme.colorScheme
    val bg = if (destructive) cs.errorContainer else st.successContainer
    val fg = if (destructive) cs.onErrorContainer else st.onSuccessContainer
    Surface(modifier.fillMaxWidth().testTag(tag ?: if (destructive) "tool_header_destructive" else "tool_header_readonly"), color = bg, contentColor = fg, shape = MaterialTheme.shapes.medium) {
        Row(Modifier.padding(14.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(painterResource(if (destructive) R.drawable.ic_warning else R.drawable.ic_dt_lock), contentDescription = null, Modifier.size(24.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(title, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                    Chip(stringResource(if (destructive) R.string.dt_badge_write else R.string.dt_badge_read_only), severity = if (destructive) Severity.Error else Severity.Success)
                }
                Text(text, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

/** チップや短いラベルを、幅に収まらなければ折り返して並べる (大きい文字・横向きでも切れない)。 */
@OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
@Composable
fun ChipFlow(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    androidx.compose.foundation.layout.FlowRow(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) { content() }
}

/** 書き込む操作の実行ボタン (赤)。 */
@Composable
fun DangerButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    Button(
        onClick = onClick, enabled = enabled, modifier = modifier.fillMaxWidth(),
        colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error, contentColor = MaterialTheme.colorScheme.onError),
    ) { Text(text) }
}

/** 書き込む操作の「計画 / 準備」側のボタン (赤い縁。まだ何も書かない)。 */
@Composable
fun DangerOutlinedButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    OutlinedButton(
        onClick = onClick, enabled = enabled, modifier = modifier.fillMaxWidth(),
        border = BorderStroke(1.dp, if (enabled) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outlineVariant),
        colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
    ) { Text(text) }
}

/** 破壊的な操作のまとまり (赤い縁のカード)。読み取り専用の表示 ([SectionCard]) と並べても区別できる。 */
@Composable
fun DangerCard(title: String, modifier: Modifier = Modifier, subtitle: String? = null, content: @Composable () -> Unit) {
    Surface(
        modifier.fillMaxWidth(), shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surfaceContainerLow,
        border = BorderStroke(2.dp, MaterialTheme.colorScheme.error),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Icon(painterResource(R.drawable.ic_warning), contentDescription = null, tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(22.dp))
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.error)
                    if (subtitle != null) Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            content()
        }
    }
}

// ================================================================ 確認ダイアログ (文字列入力)

/**
 * 破壊的な操作の最終確認。対象 ([subject]: デバイス名・パーティション・容量など) を並べ、ネイティブが決めた確認文字列 ([phrase]) を**打たせる**
 * (長押しや OK だけでは進めない)。打った文字が一致するまで実行ボタンは無効。キャンセル・外側のタップでは何も変えない。
 */
@Composable
fun TypedConfirmDialog(
    title: String, warning: String, subject: List<Pair<String, String>>, phrase: String, confirmLabel: String,
    onConfirm: (String) -> Unit, onCancel: () -> Unit,
) {
    var typed by remember { mutableStateOf("") }
    val match = typed.trim() == phrase
    AlertDialog(
        onDismissRequest = onCancel,
        modifier = Modifier.padding(horizontal = 24.dp),
        properties = androidx.compose.ui.window.DialogProperties(usePlatformDefaultWidth = false),
        icon = { Icon(painterResource(R.drawable.ic_warning), contentDescription = null, tint = MaterialTheme.colorScheme.error) },
        title = { Text(title, color = MaterialTheme.colorScheme.error) },
        text = {
            Column(Modifier.testTag("disk_confirm_body").verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(warning, style = MaterialTheme.typography.bodyMedium)
                Column(Modifier.testTag("disk_confirm_subject"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    subject.forEach { (k, v) -> KeyValue(k, v) }
                }
                Text(stringResource(R.string.dt_confirm_type_prompt), style = MaterialTheme.typography.bodySmall)
                Text(phrase, style = MaterialTheme.typography.titleMedium, fontFamily = FontFamily.Monospace, modifier = Modifier.testTag("disk_confirm_phrase"))
                OutlinedTextField(
                    value = typed, onValueChange = { typed = it }, singleLine = true, modifier = Modifier.fillMaxWidth().testTag("disk_confirm_input"),
                    label = { Text(stringResource(R.string.dt_confirm_input_label)) }, isError = typed.isNotEmpty() && !match,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(typed.trim()) }, enabled = match, modifier = Modifier.testTag("disk_confirm_ok")) {
                Text(confirmLabel, color = if (match) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        dismissButton = { TextButton(onClick = onCancel, modifier = Modifier.testTag("disk_confirm_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

// ================================================================ ジョブ (進捗・取り消し)

/**
 * 長い処理を始める。ネイティブの処理は取り消せないところで止められないので、画面が消えても (スコープが取り消されても) 最後まで走らせてから後始末する
 * (処理中にジョブを閉じない)。画面が消えたときは [done] を呼ばない。[onJob] は処理の間だけ非 null (画面が進捗・取り消しに使う)。
 */
fun <T> CoroutineScope.launchJob(newJob: () -> DiskOpJob, onJob: (DiskOpJob?) -> Unit, work: (DiskOpJob) -> T, done: (T) -> Unit) {
    val job = newJob()
    onJob(job)
    launch {
        try {
            val r = withContext(NonCancellable + Dispatchers.IO) { work(job) }
            done(r)
        } finally {
            onJob(null)
            job.close()
        }
    }
}

/** 画面が消えるとき、実行中のジョブを取り消す (処理は止まってから後始末される)。 */
@Composable
fun CancelOnDispose(job: () -> DiskOpJob?) {
    DisposableEffect(Unit) { onDispose { job()?.cancel() } }
}

/** ジョブの進捗を 200ms ごとに読む (ジョブが null の間は止まる)。 */
@Composable
fun jobProgress(job: DiskOpJob?): OpProgress? {
    var p by remember(job) { mutableStateOf<OpProgress?>(null) }
    LaunchedEffectUi(job) {
        if (job != null) while (true) { p = job.progress(); delay(200) }
    }
    return p
}

/** 進捗の棒と、段階名・割合。[total] が 0 のときは不定の棒。 */
@Composable
fun ProgressRow(p: OpProgress?, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth().testTag("disk_progress"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (p != null && p.total > 0) {
            val f = (p.done.toFloat() / p.total.toFloat()).coerceIn(0f, 1f)
            LinearProgressIndicator(progress = { f }, modifier = Modifier.fillMaxWidth())
            Text("${p.phase.ifBlank { "…" }}  ${(f * 100).toInt()}%", style = MaterialTheme.typography.bodySmall)
        } else {
            LinearProgressIndicator(Modifier.fillMaxWidth())
            Text(p?.phase?.ifBlank { null } ?: stringResource(R.string.dt_progress_working), style = MaterialTheme.typography.bodySmall)
        }
    }
}

// ================================================================ パーティション構成のグラフ

/** グラフ・凡例の 1 区間 (パーティションか空き)。 */
data class Segment(val start: Long, val size: Long, val index: Int, val fs: String, val label: String, val encrypted: Boolean, val free: Boolean)

/** パーティション表を、ディスクの先頭から順の区間 (パーティション + 空き領域) にする。1 MiB 未満の隙間 (アラインメント) は空きとして出さない。 */
fun segmentsOf(t: PartitionTable): List<Segment> {
    if (t.diskBytes <= 0) return emptyList()
    val ps = t.partitions.filter { !it.extendedContainer && !it.protective && it.sizeBytes > 0 }.sortedBy { it.startBytes }
    val out = ArrayList<Segment>()
    var pos = 0L
    val minFree = 1L shl 20
    for (p in ps) {
        if (p.startBytes - pos >= minFree) out.add(Segment(pos, p.startBytes - pos, 0, "", "", false, true))
        out.add(Segment(p.startBytes, p.sizeBytes, p.index, p.fs, p.name, CryptoMount.isCryptoFs(p.fs), false))
        pos = maxOf(pos, p.startBytes + p.sizeBytes)
    }
    if (t.diskBytes - pos >= minFree) out.add(Segment(pos, t.diskBytes - pos, 0, "", "", false, true))
    return out
}

@Composable
private fun segmentColor(s: Segment): Color {
    val cs = MaterialTheme.colorScheme
    if (s.free) return Color.Transparent
    return when {
        s.encrypted -> cs.onSurfaceVariant
        s.fs.startsWith("ext") -> cs.tertiary
        s.fs == "ntfs" -> cs.secondary
        s.fs.startsWith("fat") || s.fs == "exfat" -> cs.primary
        s.fs == "unknown" || s.fs.isBlank() -> cs.outline
        else -> cs.inversePrimary
    }
}

/**
 * パーティション構成の帯グラフ (幅は容量に比例。小さい区間も見えるよう最小幅あり)。空き領域は枠線だけ、暗号化は鍵のアイコン付き。
 * TalkBack には、帯全体を 1 つの説明 (区間の数・種別・空き) として読ませる。[tag] = テスト用。
 */
@Composable
fun PartitionBar(t: PartitionTable, tag: String, modifier: Modifier = Modifier) {
    val segs = remember(t) { segmentsOf(t) }
    val free = segs.filter { it.free }.sumOf { it.size }
    val desc = stringResource(R.string.dt_bar_desc, t.kind.uppercase(), formatBytes(t.diskBytes), segs.count { !it.free }, formatBytes(free)) +
        segs.filter { !it.free }.map { " " + stringResource(R.string.dt_bar_desc_part, it.index, fsText(it.fs), formatBytes(it.size)) }.joinToString("")
    Row(
        modifier.fillMaxWidth().height(44.dp).clip(MaterialTheme.shapes.small).testTag(tag).clearAndSetSemantics { contentDescription = desc },
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        if (segs.isEmpty()) {
            Box(Modifier.weight(1f).height(44.dp).border(1.dp, MaterialTheme.colorScheme.outline, MaterialTheme.shapes.small))
        }
        segs.forEach { s ->
            val w = (s.size.toDouble() / t.diskBytes.toDouble()).toFloat().coerceAtLeast(0.06f)
            val c = segmentColor(s)
            Box(
                Modifier.weight(w).height(44.dp).clip(MaterialTheme.shapes.extraSmall)
                    .then(if (s.free) Modifier.border(1.dp, MaterialTheme.colorScheme.outline, MaterialTheme.shapes.extraSmall) else Modifier.background(c)),
                contentAlignment = Alignment.Center,
            ) {
                if (!s.free) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (s.encrypted) Icon(painterResource(R.drawable.ic_dt_lock), contentDescription = null, tint = MaterialTheme.colorScheme.surface, modifier = Modifier.size(14.dp))
                        Text("#${s.index}", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.surface, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
    }
}

/** 凡例 (区間ごとの行)。 */
@Composable
fun PartitionLegend(t: PartitionTable, tag: String) {
    val segs = remember(t) { segmentsOf(t) }
    Column(Modifier.testTag(tag), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        segs.forEach { s ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (s.free) {
                    Text(stringResource(R.string.dt_legend_free), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                    Chip(stringResource(R.string.dt_legend_free_chip))
                } else {
                    Text("#${s.index} ${s.label}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                    Chip(fsText(s.fs))
                    if (s.encrypted) Chip(stringResource(R.string.crypto_chip_encrypted))
                }
                Text(formatBytes(s.size), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        if (segs.isEmpty()) Text(stringResource(R.string.dt_legend_empty), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

// ================================================================ 小物

/** ネイティブの JSON の 64 ビット値 (数値または文字列) を Long に。 */
fun JSONObject.long(name: String): Long = optString(name, "0").toLongOrNull() ?: 0L

/** 診断の配列 ({severity, code, message}) を (重さ, 文言) に。 */
fun diagnostics(a: JSONArray?): List<Pair<Severity, String>> = if (a == null) emptyList() else List(a.length()) { i ->
    val o = a.optJSONObject(i)
    if (o == null) Severity.Info to a.optString(i)
    else (when (o.optString("severity").lowercase()) { "error" -> Severity.Error; "warning" -> Severity.Warning; else -> Severity.Info }) to o.optString("message", o.toString())
}

/** 診断の 1 行 (重さのアイコンと色)。 */
@Composable
fun DiagLine(sev: Severity, text: String, modifier: Modifier = Modifier) {
    val cs = MaterialTheme.colorScheme
    val color = when (sev) { Severity.Error -> cs.error; Severity.Warning -> LocalStatusColors.current.onWarningContainer; else -> cs.onSurfaceVariant }
    val icon = when (sev) { Severity.Error -> R.drawable.ic_error; Severity.Warning -> R.drawable.ic_warning; else -> R.drawable.ic_info }
    val sevText = stringResource(when (sev) { Severity.Error -> R.string.dt_sev_error; Severity.Warning -> R.string.dt_sev_warning; else -> R.string.dt_sev_info })
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.Top) {
        Icon(painterResource(icon), contentDescription = sevText, tint = color, modifier = Modifier.size(18.dp))
        Text(text, style = MaterialTheme.typography.bodySmall, color = color, modifier = Modifier.weight(1f))
    }
}
