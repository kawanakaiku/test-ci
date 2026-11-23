// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.usbmem.app.MkfsPlan
import org.usbmem.app.R
import org.usbmem.app.formatBytes
import java.security.SecureRandom

/**
 * フォーマット (mkfs。docs/disk-tools.md 3 章・docs/mkfs-extended.md)。流れ: 範囲・種類・ラベル・サイズを選ぶ → 計画 (まだ何も書かない) → 確認文字列を打つ → 適用 → 読み戻しの検証。
 * 選択肢 (作れる FS・容量の範囲・使えるクラスタサイズ・ラベルの上限・ext の機能) は、ネイティブの容量・機能の問い合わせ (`MkfsSession.capabilities`) の結果から出す。
 * 「すべて許可」で開いていないと実行できない。
 */
@Composable
fun DiskFormatScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { rangesOf(c.table, "") }
    var sel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val range = ranges.firstOrNull { it.index == sel } ?: ranges.first()
    var caps by remember { mutableStateOf<List<FormatCap>>(emptyList()) }
    var fs by rememberSaveable { mutableStateOf("") }
    var label by rememberSaveable { mutableStateOf("") }
    var cluster by rememberSaveable { mutableIntStateOf(0) }
    var seedText by rememberSaveable { mutableStateOf("") }
    var advanced by rememberSaveable { mutableStateOf(false) }
    val toggled = remember { mutableStateListOf<String>() }
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

    // 範囲が変わったら、マウント中か・作れる形式と容量の可否・使えるクラスタサイズを取り直す (ネイティブの問い合わせ。何も開かない・書かない)
    LaunchedEffectUi(range.index, c.table) { inUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, range) } }
    LaunchedEffectUi(range.index, c.table) { caps = withContext(Dispatchers.IO) { c.a.formatCaps(range, c.table.sectorSize) } }

    fun canMake(name: String): Boolean = caps.firstOrNull { it.fs == name }?.sizeOk == true
    val fatAuto = if (caps.any { it.fs.startsWith("fat") }) FormatChoices.autoFat(range.size) { canMake(it) } else null
    val effFs = if (fs == FormatChoices.FAT_AUTO) fatAuto ?: "" else fs
    val cap = caps.firstOrNull { it.fs == effFs }
    val fitsNow = if (fs == FormatChoices.FAT_AUTO) fatAuto != null else cap?.sizeOk == true
    val clusterChoices = listOf(0) + (cap?.clusterOk ?: emptyList())

    // 選択肢が揃ったら、作れる種類のうち最初のものを選んでおく (空の画面で何も選ばれていない状態にしない)。
    // 選んだあとで範囲を変えて作れなくなったときは、選びなおさず理由を見せる (作れない形式のチップも押せる。計画のボタンだけが止まる)
    LaunchedEffectUi(caps, range.index) { if (fs.isEmpty()) caps.firstOrNull { it.sizeOk }?.let { fs = it.fs } }
    // 形式が変わったら、その形式で使えないクラスタサイズ・ext の機能・ラベルの文字を直す
    LaunchedEffectUi(effFs, caps) {
        if (cluster !in clusterChoices) cluster = 0
        toggled.clear()
        label = FormatChoices.sanitizeLabel(label, cap)
    }

    val features = if (cap != null && cap.features.isNotEmpty()) FormatChoices.featureSpec(cap.features, toggled.toSet()) else ""
    fun spec(seed: Long, ts: Long) = FormatSpec(effFs, FormatChoices.sanitizeLabel(label.trim(), cap), cluster, seed, ts, features)
    val inputKey = listOf(range.index, fs, effFs, label.trim(), cluster, seedText.trim(), features)
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
            ChipFlow {
                caps.forEach { cp ->
                    if (cp.fs == "fat12") {
                        FilterChip(
                            selected = fs == FormatChoices.FAT_AUTO, onClick = { fs = FormatChoices.FAT_AUTO }, enabled = !running && fatAuto != null,
                            label = { Text(stringResource(R.string.dt_format_fat_auto)) }, modifier = Modifier.testTag("fmt_fs_fat_auto"),
                        )
                    }
                    FilterChip(
                        selected = fs == cp.fs, onClick = { fs = cp.fs }, enabled = !running,
                        label = { Text(fsChipText(cp)) }, modifier = Modifier.testTag("fmt_fs_${cp.fs}"),
                    )
                }
            }
            if (fs == FormatChoices.FAT_AUTO) {
                if (fatAuto != null) Text(stringResource(R.string.dt_format_fat_auto_pick, formatBytes(range.size), fatAuto.uppercase()), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("fmt_fat_auto_pick"))
                else Text(stringResource(R.string.dt_format_fat_auto_none), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("fmt_fat_auto_none"))
            }
            if (cap != null) {
                Text(
                    stringResource(R.string.dt_format_cap_range, formatBytes(cap.minBytes), capMax(cap), formatBytes(range.size)),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("fmt_cap_range"),
                )
                if (!cap.sizeOk) Banner(
                    Notice(Severity.Warning, capReason(cap), cap.reason.ifBlank { null }),
                    Modifier.testTag("fmt_cap_reason"),
                )
                if (cap.experimental) Banner(
                    Notice(Severity.Warning, stringResource(R.string.dt_format_ntfs_experimental), stringResource(R.string.dt_format_ntfs_experimental_detail)),
                    Modifier.testTag("fmt_ntfs_experimental"),
                )
            }
            if (caps.isNotEmpty() && caps.none { it.sizeOk }) Text(stringResource(R.string.dt_format_too_small), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("fmt_none_fits"))

            OutlinedTextField(
                value = label, onValueChange = { label = FormatChoices.sanitizeLabel(it, cap) }, singleLine = true, enabled = !running,
                label = { Text(stringResource(R.string.dt_format_label)) }, supportingText = { Text(labelHint(cap)) },
                modifier = Modifier.fillMaxWidth().testTag("fmt_label"),
            )

            Text(stringResource(if (effFs.startsWith("ext")) R.string.dt_format_block else R.string.dt_format_cluster), style = MaterialTheme.typography.titleSmall)
            ChipFlow {
                clusterChoices.forEach { cs ->
                    FilterChip(
                        selected = cluster == cs, onClick = { cluster = cs }, enabled = !running,
                        label = {
                            Text(
                                if (cs != 0) formatBytes(cs.toLong())
                                else if (cap != null && cap.defaultCluster > 0) stringResource(R.string.dt_format_auto_with, formatBytes(cap.defaultCluster.toLong()))
                                else stringResource(R.string.dt_format_auto),
                            )
                        },
                        modifier = Modifier.testTag("fmt_cluster_$cs"),
                    )
                }
            }
            OutlinedTextField(
                value = seedText, onValueChange = { v -> seedText = v.filter { it.isLetterOrDigit() }.take(16) }, singleLine = true, enabled = !running,
                label = { Text(stringResource(R.string.dt_format_seed)) }, supportingText = { Text(stringResource(R.string.dt_format_seed_hint)) },
                modifier = Modifier.fillMaxWidth().testTag("fmt_seed"),
            )

            // ext3 / ext4 の機能 (詳細設定。折りたたむ)
            if (cap != null && cap.features.isNotEmpty()) {
                TextButton(onClick = { advanced = !advanced }, modifier = Modifier.testTag("fmt_adv_toggle")) {
                    Text(stringResource(if (advanced) R.string.dt_format_adv_hide else R.string.dt_format_adv_show))
                }
                if (advanced) Column(Modifier.testTag("fmt_adv"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(stringResource(R.string.dt_format_adv_text), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    cap.features.forEach { f ->
                        val name = f.removePrefix("^")
                        val on = (!f.startsWith("^")) != (name in toggled)
                        Row(
                            Modifier.fillMaxWidth().toggleable(value = on, enabled = !running, role = Role.Checkbox, onValueChange = { if (name in toggled) toggled.remove(name) else toggled.add(name) }).testTag("fmt_feat_$name"),
                            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Checkbox(checked = on, onCheckedChange = null, enabled = !running)
                            Text(name, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
                        }
                    }
                }
            }
        }

        // ---- 計画 (まだ何も書かない)
        DangerOutlinedButton(
            stringResource(R.string.dt_plan_button), enabled = effFs.isNotEmpty() && fitsNow && !running && !planning,
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

/** 形式のチップの文字 (exFAT は大小の混じった表記、NTFS は実験的の注記つき)。 */
@Composable
private fun fsChipText(cp: FormatCap): String {
    val base = if (cp.fs == "exfat") "exFAT" else cp.fs.uppercase()
    return if (cp.experimental) stringResource(R.string.dt_format_fs_experimental, base) else base
}

/** 作れない理由 (ネイティブの理由コード → 文言。容量の下限・上限は問い合わせの値を入れる)。詳しい英語の説明は、あとに付ける。 */
@Composable
private fun capReason(cp: FormatCap): String = when (cp.reasonCode) {
    "too-small" -> stringResource(R.string.dt_format_cap_too_small, formatBytes(cp.minBytes))
    "too-large" -> stringResource(R.string.dt_format_cap_too_large, capMax(cp))
    else -> diskCode(cp.reasonCode).let { if (it == cp.reasonCode) stringResource(R.string.dt_format_cap_other) else it }
}

/** 作れる最大の容量の文字 (1 PiB 以上は、実質制限なし)。 */
@Composable
private fun capMax(cp: FormatCap): String = if (cp.maxBytes <= 0L || cp.maxBytes >= (1L shl 50)) stringResource(R.string.dt_format_unlimited) else formatBytes(cp.maxBytes)

@Composable
private fun labelHint(cap: FormatCap?): String {
    if (cap == null || cap.labelMax <= 0) return stringResource(R.string.dt_format_label_hint)
    return when (cap.labelCharset) {
        "ascii-upper" -> stringResource(R.string.dt_format_label_ascii, cap.labelMax)
        "utf8" -> stringResource(R.string.dt_format_label_utf8, cap.labelMax)
        else -> stringResource(R.string.dt_format_label_utf16, cap.labelMax)
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
