// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.usbmem.app.MkfsPlan
import org.usbmem.app.R
import org.usbmem.app.formatBytes
import java.security.SecureRandom

/**
 * フォーマット (mkfs。docs/disk-tools.md 3 章)。流れ: 範囲・種類・ラベル・サイズを選ぶ → 計画 (まだ何も書かない) → 確認文字列を打つ → 適用 → 読み戻しの検証。
 * 選択肢 (作れる FS・最小サイズ・使えるクラスタサイズ) は、ネイティブへの問い合わせの結果から出す。「すべて許可」で開いていないと実行できない。
 */
@Composable
fun DiskFormatScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { rangesOf(c.table, "") }
    var sel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val range = ranges.firstOrNull { it.index == sel } ?: ranges.first()
    var choices by remember { mutableStateOf<List<Pair<String, Long>>>(emptyList()) }
    var fs by rememberSaveable { mutableStateOf("") }
    var label by rememberSaveable { mutableStateOf("") }
    var cluster by rememberSaveable { mutableIntStateOf(0) }
    var clusterChoices by remember { mutableStateOf(listOf(0)) }
    var seedText by rememberSaveable { mutableStateOf("") }
    var plan by remember { mutableStateOf<MkfsPlan?>(null) }
    var planSpec by remember { mutableStateOf<FormatSpec?>(null) }
    var planError by remember { mutableStateOf<String?>(null) }
    var planning by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf(false) }
    var done by remember { mutableStateOf<FormatDone?>(null) }
    var applyError by remember { mutableStateOf<String?>(null) }
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    var inUse by remember { mutableStateOf(false) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    val running = job != null

    LaunchedEffectUi(Unit) { choices = withContext(Dispatchers.IO) { c.a.formatChoices() } }
    // 範囲が変わったら、マウント中か・サイズの足りる種類・使えるクラスタサイズを取り直す
    LaunchedEffectUi(range.index, c.table) { inUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, range) } }
    val fits = choices.filter { it.second <= range.size }
    LaunchedEffectUi(range.index, fs) {
        if (fs.isNotEmpty()) {
            clusterChoices = withContext(Dispatchers.IO) { c.a.formatClusterChoices(c.target, range, fs) }
            if (cluster !in clusterChoices) cluster = 0
        }
    }
    // 選択肢が揃ったら、入る種類のうち最初のものを選んでおく (空の画面で何も選ばれていない状態にしない)
    LaunchedEffectUi(choices, range.index) { if (fits.none { it.first == fs }) fits.firstOrNull()?.let { fs = it.first } }

    fun spec(seed: Long, ts: Long) = FormatSpec(fs, label.trim(), cluster, seed, ts)
    val inputKey = listOf(range.index, fs, label.trim(), cluster, seedText.trim())
    var planKey by remember { mutableStateOf<List<Any>?>(null) }
    val planValid = plan != null && planKey == inputKey

    fun newSeed(): Long {
        val typed = seedText.trim().removePrefix("0x")
        if (typed.isNotEmpty()) return typed.toLongOrNull(16)?.takeIf { it != 0L } ?: 1L
        var v = SecureRandom().nextLong()
        if (v == 0L) v = 1L
        return v
    }

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        ToolHeader(true, stringResource(R.string.dt_format_header), stringResource(R.string.dt_format_header_text))

        if (!c.writeAllowed) Banner(
            Notice(Severity.Warning, stringResource(R.string.dt_need_write_title), stringResource(if (c.target.kind == DiskTargetKind.Usb) R.string.dt_need_write_usb else R.string.dt_need_write_image)),
            Modifier.testTag("fmt_need_write"),
        )

        SectionCard(stringResource(R.string.dt_format_target)) {
            RangePicker(
                ranges.map { if (it.whole) it.copy(label = stringResource(R.string.dt_range_whole)) else it }, sel,
                onSelect = { sel = it.index; plan = null; done = null; applyError = null }, enabled = { !running },
            )
            if (inUse) Banner(Notice(Severity.Error, stringResource(R.string.dt_in_use_title), stringResource(R.string.dt_in_use_format)), Modifier.testTag("fmt_in_use"))
            if (range.whole && c.table.partitions.any { !it.extendedContainer && !it.protective } && !c.table.wholeDeviceVolume)
                Banner(Notice(Severity.Warning, stringResource(R.string.dt_format_whole_warn)), Modifier.testTag("fmt_whole_warn"))
        }

        SectionCard(stringResource(R.string.dt_format_options)) {
            Text(stringResource(R.string.dt_format_fs), style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                choices.forEach { (name, min) ->
                    FilterChip(
                        selected = fs == name, onClick = { fs = name }, enabled = !running && min <= range.size,
                        label = { Text(name.uppercase()) }, modifier = Modifier.testTag("fmt_fs_$name"),
                    )
                }
            }
            choices.firstOrNull { it.first == fs }?.let { Text(stringResource(R.string.dt_format_min_size, formatBytes(it.second)), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            if (choices.isNotEmpty() && fits.isEmpty()) Text(stringResource(R.string.dt_format_too_small), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)

            OutlinedTextField(
                value = label, onValueChange = { label = it }, singleLine = true, enabled = !running,
                label = { Text(stringResource(R.string.dt_format_label)) }, supportingText = { Text(stringResource(R.string.dt_format_label_hint)) },
                modifier = Modifier.fillMaxWidth().testTag("fmt_label"),
            )

            Text(stringResource(if (fs.startsWith("ext")) R.string.dt_format_block else R.string.dt_format_cluster), style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                clusterChoices.forEach { cs ->
                    FilterChip(
                        selected = cluster == cs, onClick = { cluster = cs }, enabled = !running,
                        label = { Text(if (cs == 0) stringResource(R.string.dt_format_auto) else formatBytes(cs.toLong())) }, modifier = Modifier.testTag("fmt_cluster_$cs"),
                    )
                }
            }
            OutlinedTextField(
                value = seedText, onValueChange = { v -> seedText = v.filter { it.isLetterOrDigit() }.take(16) }, singleLine = true, enabled = !running,
                label = { Text(stringResource(R.string.dt_format_seed)) }, supportingText = { Text(stringResource(R.string.dt_format_seed_hint)) },
                modifier = Modifier.fillMaxWidth().testTag("fmt_seed"),
            )
        }

        // ---- 計画 (まだ何も書かない)
        DangerOutlinedButton(
            stringResource(R.string.dt_plan_button), enabled = fs.isNotEmpty() && !running && !planning && fits.isNotEmpty(),
            modifier = Modifier.testTag("fmt_plan"),
            onClick = {
                plan = null; planError = null; done = null; applyError = null; planning = true
                val sp = spec(newSeed(), System.currentTimeMillis() / 1000)
                val key = inputKey
                scope.launchJob(
                    newJob = { c.a.newCheckJob() }, onJob = { },
                    work = { _ -> c.a.formatPlan(c.target, range, sp) },
                    done = { o -> plan = o.value; planError = o.error; planSpec = sp; planKey = key; planning = false },
                )
            },
        )
        planError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_plan_failed), it), Modifier.testTag("fmt_plan_error")) }
        val p = plan
        val sp = planSpec
        if (p != null && sp != null && planValid) PlanCard(p, range, c, inUse, running, onApply = { confirm = true })

        if (running) {
            ProgressRow(progress)
            OutlinedButton(onClick = { job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("fmt_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
        }
        applyError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("fmt_apply_error"), scrollIntoView = true) }
        done?.let { d -> FormatResult(d, onVerify = { c.openTool(DiskTool.Check, range.index) }) }

        if (confirm && p != null && sp != null) {
            val oldFs = if (p.existingFs.isBlank()) stringResource(R.string.dt_fs_unknown) else p.existingFs.uppercase()
            TypedConfirmDialog(
                title = stringResource(R.string.dt_format_confirm_title),
                warning = stringResource(R.string.dt_format_confirm_warn),
                subject = listOf(
                    stringResource(R.string.dt_subject_device) to c.target.title,
                    stringResource(R.string.dt_subject_range) to (if (range.whole) stringResource(R.string.dt_range_whole) else range.label) + " (${fsText(range.fs)})",
                    stringResource(R.string.dt_subject_size) to formatBytes(range.size),
                    stringResource(R.string.dt_subject_new_fs) to sp.fs.uppercase(),
                    stringResource(R.string.dt_subject_old_fs) to oldFs,
                ),
                phrase = p.confirmPhrase, confirmLabel = stringResource(R.string.dt_format_do),
                onCancel = { confirm = false },
                onConfirm = { typed ->
                    confirm = false; done = null; applyError = null
                    scope.launchJob(
                        newJob = { c.a.newCheckJob() }, onJob = { job = it },
                        work = { j -> c.a.formatApply(c.target, range, sp, p.confirmPhrase, typed, j) },
                        done = { o -> done = o.value; applyError = o.error; plan = null; c.reload() },
                    )
                },
            )
        }
    }
}

@Composable
private fun PlanCard(p: MkfsPlan, range: DiskRange, c: ToolContext, inUse: Boolean, running: Boolean, onApply: () -> Unit) {
    val problems = remember(p) { diagnostics(p.json.optJSONArray("problems")) }
    SectionCard(stringResource(R.string.dt_plan_title), subtitle = stringResource(R.string.dt_plan_sub), modifier = Modifier.testTag("fmt_plan_card")) {
        if (p.existingFs.isNotBlank()) Banner(Notice(Severity.Warning, stringResource(R.string.dt_format_existing, p.existingFs.uppercase()), stringResource(R.string.dt_format_existing_detail)), Modifier.testTag("fmt_existing"))
        problems.forEach { (sev, text) -> DiagLine(sev, text) }
        KeyValue(stringResource(R.string.dt_plan_confirm_phrase), p.confirmPhrase)
        KeyValue(stringResource(R.string.dt_plan_bytes), formatBytes(p.bytesToWrite))
        // 作られる構造 (ネイティブの計画そのまま: クラスタサイズ・UUID / シリアルなど)
        Column(Modifier.testTag("fmt_layout"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            p.layout.forEach { (k, v) -> KeyValue(k, v) }
        }
        p.changes.forEach { Text("• $it", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        Text(stringResource(R.string.dt_format_backup_note), style = MaterialTheme.typography.bodySmall)
        DangerButton(
            stringResource(R.string.dt_format_do_button), onClick = onApply, modifier = Modifier.testTag("fmt_apply"),
            enabled = p.ok && c.writeAllowed && !inUse && !running,
        )
        if (!p.ok) Text(stringResource(R.string.dt_plan_not_ok), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
    }
}

@Composable
private fun FormatResult(d: FormatDone, onVerify: () -> Unit) {
    val r = d.result
    Column(Modifier.testTag("fmt_result"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (r.ok) {
            Banner(
                Notice(if (r.verified) Severity.Success else Severity.Warning, stringResource(if (r.verified) R.string.dt_format_done_verified else R.string.dt_format_done), stringResource(R.string.dt_format_done_detail, formatBytes(r.bytesWritten), r.checkVerdict)),
                Modifier.testTag("fmt_done"), scrollIntoView = true,
            )
            d.backupName?.let { Text(stringResource(R.string.dt_backup_saved, it), style = MaterialTheme.typography.bodySmall) }
            OutlinedButton(onClick = onVerify, modifier = Modifier.fillMaxWidth().testTag("fmt_verify")) { Text(stringResource(R.string.dt_format_verify)) }
        } else {
            Banner(
                Notice(Severity.Error, diskCode(r.code), (if (r.rolledBack) stringResource(R.string.dt_rolled_back) + "\n" else "") + r.message),
                Modifier.testTag("fmt_failed"), scrollIntoView = true,
            )
        }
    }
}
