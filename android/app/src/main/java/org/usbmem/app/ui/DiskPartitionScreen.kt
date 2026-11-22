// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.usbmem.app.CryptoMount
import org.usbmem.app.PartitionPlan
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes
import java.security.SecureRandom
import java.util.UUID

private const val GPT_LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
private const val GPT_BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
private const val GPT_EFI = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

/**
 * パーティション編集。上: 今の構成 (帯グラフ + 一覧。**表示のみ**・青緑系)。下: 編集 (**書き込みます**・赤系)。
 * 操作を積む → 計画 (まだ書かない。変更の一覧と適用前後の表) → 確認文字列 (WRITE xxxxxx) → 適用 → 読み戻しの検証。
 * 適用の前に、書き換える表のバックアップをアプリ内へ自動で保存する。docs/partition-edit.md
 */
@Composable
fun DiskPartitionScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val t = c.table
    val ops = remember { mutableStateListOf<PartOp>() }
    var plan by remember { mutableStateOf<PartitionPlan?>(null) }
    var planOps by remember { mutableStateOf<List<PartOp>>(emptyList()) }
    var planError by remember { mutableStateOf<String?>(null) }
    var applyError by remember { mutableStateOf<String?>(null) }
    var applied by remember { mutableStateOf<PartitionDone?>(null) }
    var confirm by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf(-1) }
    var dialog by remember { mutableStateOf<String?>(null) }   // "new" / "create" / "resize:<n>" / "rename:<n>" / "type:<n>"
    var gpt by remember { mutableStateOf<JSONObject?>(null) }
    var inUse by remember { mutableStateOf(false) }
    var backupSavedBytes by remember { mutableStateOf<Int?>(null) }
    var backupError by remember { mutableStateOf<String?>(null) }
    val isGpt = t.kind == "gpt"

    LaunchedEffectUi(t) {
        gpt = if (isGpt) withContext(Dispatchers.IO) { c.a.partitionGpt(c.target) }.value else null
        inUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, DiskRange(0, Long.MAX_VALUE / 2, "", "", 0)) }
    }
    fun invalidate() { plan = null; planError = null; applyError = null }

    val saveBackup = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri: Uri? ->
        if (uri != null) scope.launch {
            busy = true; backupSavedBytes = null; backupError = null
            val r = withContext(Dispatchers.IO) { c.a.partitionBackupTo(c.target, uri) }
            busy = false
            backupSavedBytes = r.value; backupError = r.error
        }
    }
    val pickRestore = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) {
            ops.clear(); ops.add(PartOp.RestoreTable(uri, uri.lastPathSegment?.substringAfterLast('/') ?: "backup")); invalidate(); applied = null
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ================= 今の構成 (表示だけ)
        ToolHeader(false, stringResource(R.string.dt_pt_view_header), stringResource(R.string.dt_pt_view_text))
        SectionCard(stringResource(R.string.dt_pt_current), subtitle = stringResource(R.string.common_partitions_subtitle, c.target.title, t.kind.uppercase(), formatBytes(t.diskBytes), t.sectorSize)) {
            PartitionBar(t, "pt_bar")
            PartitionLegend(t, "pt_legend")
            t.diagnostics.forEach { d -> DiagLine(if (d.severity.lowercase() == "error") Severity.Error else if (d.severity.lowercase() == "warning") Severity.Warning else Severity.Info, d.message) }
            gpt?.let { g ->
                diagnostics(g.optJSONArray("diagnostics")).forEach { (sev, m) -> DiagLine(sev, m) }
                val repairs = g.optJSONArray("suggestedRepairs")
                if (repairs != null && repairs.length() > 0) {
                    Text(stringResource(R.string.dt_pt_repairs), style = MaterialTheme.typography.titleSmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        for (i in 0 until repairs.length()) {
                            val k = repairs.optString(i)
                            OutlinedButton(onClick = { ops.add(PartOp.Repair(k)); invalidate(); applied = null }, modifier = Modifier.testTag("pt_repair_$k")) { Text(repairLabel(k)) }
                        }
                    }
                }
            }
        }
        if (t.partitions.any { CryptoMount.isCryptoFs(it.fs) }) Text(stringResource(R.string.dt_pt_encrypted_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)

        // ================= 編集 (書き込みます)
        DangerCard(stringResource(R.string.dt_pt_edit), subtitle = stringResource(R.string.dt_pt_edit_sub)) {
            if (!c.writeAllowed) Banner(
                Notice(Severity.Warning, stringResource(R.string.dt_need_write_title), stringResource(if (c.target.kind == DiskTargetKind.Usb) R.string.dt_need_write_usb else R.string.dt_need_write_image)),
                Modifier.testTag("pt_need_write"),
            )
            if (inUse) Banner(Notice(Severity.Error, stringResource(R.string.dt_in_use_title), stringResource(R.string.dt_in_use_partition)), Modifier.testTag("pt_in_use"))
            Text(stringResource(R.string.dt_pt_backup_guide), style = MaterialTheme.typography.bodySmall)

            val parts = t.partitions.filter { !it.extendedContainer && !it.protective && it.sizeBytes > 0 }
            parts.forEach { p ->
                Column(Modifier.fillMaxWidth().clickable { selected = if (selected == p.index) -1 else p.index }.testTag("pt_row_${p.index}"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f, fill = false))
                        Chip(fsText(p.fs))
                        Text(formatBytes(p.sizeBytes), style = MaterialTheme.typography.bodySmall)
                    }
                    if (selected == p.index) {
                        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            TextButton(onClick = { ops.add(PartOp.Remove(p.index)); invalidate(); applied = null; selected = -1 }, modifier = Modifier.testTag("pt_remove_${p.index}")) { Text(stringResource(R.string.dt_pt_remove), color = MaterialTheme.colorScheme.error) }
                            TextButton(onClick = { dialog = "resize:${p.index}" }, modifier = Modifier.testTag("pt_resize_${p.index}")) { Text(stringResource(R.string.dt_pt_resize)) }
                            if (isGpt) TextButton(onClick = { dialog = "rename:${p.index}" }, modifier = Modifier.testTag("pt_rename_${p.index}")) { Text(stringResource(R.string.dt_pt_rename)) }
                            TextButton(onClick = { dialog = "type:${p.index}" }, modifier = Modifier.testTag("pt_type_${p.index}")) { Text(stringResource(R.string.dt_pt_type)) }
                        }
                    }
                }
            }
            if (parts.isEmpty()) Text(stringResource(R.string.dt_legend_empty), style = MaterialTheme.typography.bodySmall)
            val free = segmentsOf(t).filter { it.free }.sumOf { it.size }
            Text(stringResource(R.string.dt_pt_free, formatBytes(free)), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("pt_free"))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                DangerOutlinedButton(stringResource(R.string.dt_pt_add), onClick = { dialog = "create" }, modifier = Modifier.weight(1f).testTag("pt_add"), enabled = free > 0 && t.kind != "none")
                DangerOutlinedButton(stringResource(R.string.dt_pt_new_table), onClick = { dialog = "new" }, modifier = Modifier.weight(1f).testTag("pt_new_table"))
            }
            OutlinedButton(onClick = { pickRestore.launch(arrayOf("*/*")) }, modifier = Modifier.fillMaxWidth().testTag("pt_restore_pick")) { Text(stringResource(R.string.dt_pt_restore_table)) }

            // 保留中の変更
            if (ops.isNotEmpty()) {
                Text(stringResource(R.string.dt_pt_pending, ops.size), style = MaterialTheme.typography.titleSmall)
                Column(Modifier.testTag("pt_ops"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    ops.forEachIndexed { i, op -> Text("${i + 1}. " + opText(op), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.testTag("pt_op_$i")) }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { ops.removeAt(ops.size - 1); invalidate() }, modifier = Modifier.testTag("pt_undo")) { Text(stringResource(R.string.dt_pt_undo)) }
                    TextButton(onClick = { ops.clear(); invalidate() }, modifier = Modifier.testTag("pt_clear")) { Text(stringResource(R.string.dt_pt_clear)) }
                }
            }
            DangerOutlinedButton(
                stringResource(R.string.dt_plan_button), enabled = ops.isNotEmpty() && !busy, modifier = Modifier.testTag("pt_plan"),
                onClick = {
                    invalidate(); applied = null; busy = true
                    val snap = ops.toList()
                    scope.launch {
                        val r = withContext(Dispatchers.IO) { c.a.partitionPlan(c.target, snap) }
                        plan = r.value; planOps = snap; planError = r.error; busy = false
                    }
                },
            )
            planError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_plan_failed), it), Modifier.testTag("pt_plan_error")) }
            plan?.let { p -> PlanView(p, c, inUse, busy, onApply = { confirm = true }) }
            applyError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("pt_apply_error"), scrollIntoView = true) }
            applied?.let { d ->
                val r = d.result
                if (r.ok) {
                    Banner(
                        Notice(if (r.verified) Severity.Success else Severity.Warning, stringResource(if (r.verified) R.string.dt_pt_done_verified else R.string.dt_pt_done), stringResource(R.string.dt_pt_done_detail, r.sectorsWritten)),
                        Modifier.testTag("pt_done"), scrollIntoView = true,
                    )
                    d.backupName?.let { Text(stringResource(R.string.dt_backup_saved, it), style = MaterialTheme.typography.bodySmall) }
                } else {
                    Banner(Notice(Severity.Error, diskCode(r.code), (if (r.rolledBack) stringResource(R.string.dt_rolled_back) + "\n" else "") + r.message), Modifier.testTag("pt_failed"), scrollIntoView = true)
                }
            }
            if (busy) ProgressRow(null)
        }

        // ================= 表のバックアップ (読み取りだけ。保存先のファイルを書く)
        SectionCard(stringResource(R.string.dt_pt_backup_title), subtitle = stringResource(R.string.dt_pt_backup_sub)) {
            OutlinedButton(onClick = { saveBackup.launch("usbmem-partition-table.bak") }, enabled = !busy, modifier = Modifier.fillMaxWidth().testTag("pt_backup_save")) { Text(stringResource(R.string.dt_pt_backup_save)) }
            backupSavedBytes?.let { Text(stringResource(R.string.dt_pt_backup_saved, it), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("pt_backup_saved")) }
            backupError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("pt_backup_error")) }
        }

        // ================= 他の担当の機能の口 (登録があるときだけ出る)
        DiskFeatureSlots(c)
    }

    // ---- ダイアログ
    val d = dialog
    if (d != null) {
        val idx = d.substringAfter(':', "").toIntOrNull() ?: -1
        val part = t.partitions.firstOrNull { it.index == idx }
        when (d.substringBefore(':')) {
            "new" -> NewTableDialog(t.wholeDeviceVolume, { dialog = null }) { gptKind, over ->
                dialog = null
                ops.clear(); applied = null
                ops.add(PartOp.NewTable(gptKind, if (gptKind) UUID.randomUUID().toString().uppercase() else "0x%08x".format(SecureRandom().nextInt().let { if (it == 0) 1 else it }), over)); invalidate()
            }
            "create" -> CreateDialog(isGptNow = ops.filterIsInstance<PartOp.NewTable>().lastOrNull()?.gpt ?: isGpt, onDismiss = { dialog = null }) { sizeBytes, name, gType, mType ->
                dialog = null
                ops.add(PartOp.Create(sizeBytes, name, gType, mType, UUID.randomUUID().toString().uppercase())); invalidate(); applied = null
            }
            "resize" -> if (part != null) SizeDialog(part.sizeBytes, { dialog = null }) { b -> dialog = null; ops.add(PartOp.Resize(idx, b)); invalidate(); applied = null; selected = -1 }
            "rename" -> if (part != null) NameDialog(part.name, { dialog = null }) { n -> dialog = null; ops.add(PartOp.Rename(idx, n)); invalidate(); applied = null; selected = -1 }
            "type" -> TypeDialog(isGpt, { dialog = null }) { gType, mType -> dialog = null; ops.add(PartOp.SetType(idx, gType, mType)); invalidate(); applied = null; selected = -1 }
        }
    }

    val p = plan
    if (confirm && p != null) {
        TypedConfirmDialog(
            title = stringResource(R.string.dt_pt_confirm_title),
            warning = stringResource(R.string.dt_pt_confirm_warn),
            subject = listOf(
                stringResource(R.string.dt_subject_device) to c.target.title,
                stringResource(R.string.dt_subject_table) to "${t.kind.uppercase()} · ${formatBytes(t.diskBytes)}",
                stringResource(R.string.dt_subject_changes) to p.changes.size.toString(),
                stringResource(R.string.dt_subject_sectors) to p.sectorCount.toString(),
            ),
            phrase = p.confirmPhrase, confirmLabel = stringResource(R.string.dt_pt_apply_do),
            onCancel = { confirm = false },
            onConfirm = { typed ->
                confirm = false; busy = true; applyError = null; applied = null
                val snap = planOps
                scope.launch {
                    val r = withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { c.a.partitionApply(c.target, snap, p.confirmPhrase, typed) }
                    busy = false; applyError = r.error; applied = r.value
                    if (r.value?.result?.ok == true) { ops.clear(); plan = null }
                    c.reload()
                }
            },
        )
    }
}

@Composable
private fun repairLabel(k: String): String = when (k) {
    "copy-primary-to-backup" -> stringResource(R.string.dt_pt_repair_primary_to_backup)
    "copy-backup-to-primary" -> stringResource(R.string.dt_pt_repair_backup_to_primary)
    "move-backup-to-end" -> stringResource(R.string.dt_pt_repair_move_backup)
    else -> stringResource(R.string.dt_pt_repair_mbr)
}

@Composable
private fun opText(op: PartOp): String = when (op) {
    is PartOp.NewTable -> stringResource(R.string.dt_op_new_table, if (op.gpt) "GPT" else "MBR")
    is PartOp.Create -> stringResource(R.string.dt_op_create, if (op.sizeBytes == 0L) stringResource(R.string.dt_op_rest) else formatBytes(op.sizeBytes), op.name)
    is PartOp.Remove -> stringResource(R.string.dt_op_remove, op.index)
    is PartOp.Resize -> stringResource(R.string.dt_op_resize, op.index, formatBytes(op.sizeBytes))
    is PartOp.Rename -> stringResource(R.string.dt_op_rename, op.index, op.name)
    is PartOp.SetType -> stringResource(R.string.dt_op_type, op.index)
    is PartOp.Repair -> repairLabel(op.kind)
    is PartOp.RestoreTable -> stringResource(R.string.dt_op_restore, op.fileName)
}

@Composable
private fun PlanView(p: PartitionPlan, c: ToolContext, inUse: Boolean, busy: Boolean, onApply: () -> Unit) {
    val problems = remember(p) { diagnostics(p.json.optJSONArray("problems")) }
    val before = remember(p) { p.json.optJSONObject("before")?.let { runCatching { PartitionTable.parse(it.toString()) }.getOrNull() } }
    val after = remember(p) { p.json.optJSONObject("after")?.let { runCatching { PartitionTable.parse(it.toString()) }.getOrNull() } }
    Column(Modifier.testTag("pt_plan_card"), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(stringResource(R.string.dt_plan_title), style = MaterialTheme.typography.titleSmall)
        if (p.destructive) Banner(Notice(Severity.Warning, stringResource(R.string.dt_pt_destructive)), Modifier.testTag("pt_destructive"))
        if (p.requiresForce) Banner(Notice(Severity.Error, stringResource(R.string.dt_pt_force)))
        problems.forEach { (sev, text) -> DiagLine(sev, text) }
        p.changes.forEach { Text("• $it", style = MaterialTheme.typography.bodySmall) }
        KeyValue(stringResource(R.string.dt_plan_confirm_phrase), p.confirmPhrase)
        if (before != null) {
            Text(stringResource(R.string.dt_pt_before), style = MaterialTheme.typography.labelLarge)
            PartitionBar(before, "pt_before_bar")
            TableRows(before, "pt_before")
        }
        if (after != null) {
            Text(stringResource(R.string.dt_pt_after), style = MaterialTheme.typography.labelLarge)
            PartitionBar(after, "pt_after_bar")
            TableRows(after, "pt_after")
        }
        DangerButton(stringResource(R.string.dt_pt_apply_button), onClick = onApply, modifier = Modifier.testTag("pt_apply"), enabled = p.ok && c.writeAllowed && !inUse && !busy)
        if (!p.ok) Text(stringResource(R.string.dt_plan_not_ok), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
    }
}

/** パーティションの表 (番号・開始 LBA・サイズ・FS・名前)。 */
@Composable
fun TableRows(t: PartitionTable, tag: String) {
    Column(Modifier.testTag(tag), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row {
            listOf(R.string.dt_col_no to 0.12f, R.string.dt_col_start to 0.28f, R.string.dt_col_size to 0.24f, R.string.dt_col_fs to 0.16f, R.string.dt_col_name to 0.2f).forEach { (r, w) ->
                Text(stringResource(r), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.weight(w))
            }
        }
        t.partitions.filter { !it.extendedContainer && !it.protective }.forEach { p ->
            Row {
                Text("${p.index}", style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(0.12f))
                Text("${p.startLba}", style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(0.28f))
                Text(formatBytes(p.sizeBytes), style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(0.24f))
                Text(fsText(p.fs), style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(0.16f))
                Text(p.name, style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(0.2f))
            }
        }
        if (t.partitions.none { !it.extendedContainer && !it.protective }) Text(stringResource(R.string.dt_legend_empty), style = MaterialTheme.typography.bodySmall)
    }
}

// ================================================================ 操作のダイアログ

@Composable
private fun NewTableDialog(wholeFs: Boolean, onDismiss: () -> Unit, onOk: (Boolean, Boolean) -> Unit) {
    var gpt by remember { mutableStateOf(true) }
    var over by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss, title = { Text(stringResource(R.string.dt_pt_new_table)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.dt_pt_new_table_warn), color = MaterialTheme.colorScheme.error)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = gpt, onClick = { gpt = true }, label = { Text("GPT") }, modifier = Modifier.testTag("pt_new_gpt"))
                    FilterChip(selected = !gpt, onClick = { gpt = false }, label = { Text("MBR") }, modifier = Modifier.testTag("pt_new_mbr"))
                }
                if (wholeFs) Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = over, onCheckedChange = { over = it }, modifier = Modifier.testTag("pt_new_overwrite"))
                    Text(stringResource(R.string.dt_pt_new_overwrite), style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = { TextButton(onClick = { onOk(gpt, over) }, enabled = !wholeFs || over, modifier = Modifier.testTag("pt_dialog_ok")) { Text(stringResource(R.string.dt_pt_dialog_add)) } },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.testTag("pt_dialog_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

@Composable
private fun CreateDialog(isGptNow: Boolean, onDismiss: () -> Unit, onOk: (Long, String, String, Int) -> Unit) {
    var mib by remember { mutableStateOf("") }
    var name by remember { mutableStateOf("") }
    var kind by remember { mutableStateOf(0) }
    AlertDialog(
        onDismissRequest = onDismiss, title = { Text(stringResource(R.string.dt_pt_add)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = mib, onValueChange = { v -> mib = v.filter { it.isDigit() }.take(9) }, singleLine = true, keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(keyboardType = KeyboardType.Number),
                    label = { Text(stringResource(R.string.dt_pt_size_mib)) }, supportingText = { Text(stringResource(R.string.dt_pt_size_hint)) }, modifier = Modifier.fillMaxWidth().testTag("pt_create_size"),
                )
                if (isGptNow) OutlinedTextField(value = name, onValueChange = { name = it.take(36) }, singleLine = true, label = { Text(stringResource(R.string.dt_pt_name)) }, modifier = Modifier.fillMaxWidth().testTag("pt_create_name"))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = kind == 0, onClick = { kind = 0 }, label = { Text(stringResource(R.string.dt_type_linux)) }, modifier = Modifier.testTag("pt_type_linux"))
                    FilterChip(selected = kind == 1, onClick = { kind = 1 }, label = { Text(stringResource(R.string.dt_type_basic)) }, modifier = Modifier.testTag("pt_type_basic"))
                    if (isGptNow) FilterChip(selected = kind == 2, onClick = { kind = 2 }, label = { Text(stringResource(R.string.dt_type_efi)) }, modifier = Modifier.testTag("pt_type_efi"))
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                val bytes = (mib.toLongOrNull() ?: 0L) * (1L shl 20)
                val g = when (kind) { 1 -> GPT_BASIC; 2 -> GPT_EFI; else -> GPT_LINUX }
                val m = when (kind) { 1 -> 0x07; else -> 0x83 }
                onOk(bytes, name.trim(), g, m)
            }, modifier = Modifier.testTag("pt_dialog_ok")) { Text(stringResource(R.string.dt_pt_dialog_add)) }
        },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.testTag("pt_dialog_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

@Composable
private fun SizeDialog(current: Long, onDismiss: () -> Unit, onOk: (Long) -> Unit) {
    var mib by remember { mutableStateOf((current shr 20).toString()) }
    AlertDialog(
        onDismissRequest = onDismiss, title = { Text(stringResource(R.string.dt_pt_resize)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.dt_pt_resize_warn), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(
                    value = mib, onValueChange = { v -> mib = v.filter { it.isDigit() }.take(9) }, singleLine = true, keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(keyboardType = KeyboardType.Number),
                    label = { Text(stringResource(R.string.dt_pt_size_mib)) }, modifier = Modifier.fillMaxWidth().testTag("pt_resize_input"),
                )
            }
        },
        confirmButton = { TextButton(onClick = { onOk((mib.toLongOrNull() ?: 0L) * (1L shl 20)) }, enabled = (mib.toLongOrNull() ?: 0L) > 0, modifier = Modifier.testTag("pt_dialog_ok")) { Text(stringResource(R.string.dt_pt_dialog_add)) } },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.testTag("pt_dialog_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

@Composable
private fun NameDialog(current: String, onDismiss: () -> Unit, onOk: (String) -> Unit) {
    var name by remember { mutableStateOf(current) }
    AlertDialog(
        onDismissRequest = onDismiss, title = { Text(stringResource(R.string.dt_pt_rename)) },
        text = { OutlinedTextField(value = name, onValueChange = { name = it.take(36) }, singleLine = true, label = { Text(stringResource(R.string.dt_pt_name)) }, modifier = Modifier.fillMaxWidth().testTag("pt_rename_input")) },
        confirmButton = { TextButton(onClick = { onOk(name.trim()) }, modifier = Modifier.testTag("pt_dialog_ok")) { Text(stringResource(R.string.dt_pt_dialog_add)) } },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.testTag("pt_dialog_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

@Composable
private fun TypeDialog(isGpt: Boolean, onDismiss: () -> Unit, onOk: (String, Int) -> Unit) {
    var kind by remember { mutableStateOf(0) }
    AlertDialog(
        onDismissRequest = onDismiss, title = { Text(stringResource(R.string.dt_pt_type)) },
        text = {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = kind == 0, onClick = { kind = 0 }, label = { Text(stringResource(R.string.dt_type_linux)) })
                FilterChip(selected = kind == 1, onClick = { kind = 1 }, label = { Text(stringResource(R.string.dt_type_basic)) })
                if (isGpt) FilterChip(selected = kind == 2, onClick = { kind = 2 }, label = { Text(stringResource(R.string.dt_type_efi)) })
            }
        },
        confirmButton = { TextButton(onClick = { onOk(when (kind) { 1 -> GPT_BASIC; 2 -> GPT_EFI; else -> GPT_LINUX }, if (kind == 1) 0x07 else 0x83) }, modifier = Modifier.testTag("pt_dialog_ok")) { Text(stringResource(R.string.dt_pt_dialog_add)) } },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.testTag("pt_dialog_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}
