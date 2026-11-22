// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.usbmem.app.R
import org.usbmem.app.formatBytes

private fun displayName(u: Uri): String = u.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':') ?: u.toString()

/**
 * バックアップ / 復元 (Partclone 形式。docs/recovery-backup.md)。
 * 上: バックアップ (読み取り専用。書くのは保存先のファイルだけ)。下: 復元 (**書き込みます**: 選んだ範囲を画像の内容で上書き)。
 * 復元は 計画 → 並べて確認 (画像と書き込み先の FS・容量・ラベル) → 確認文字列 → 適用。
 */
@Composable
fun DiskBackupScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { rangesOf(c.table, "") }
    var sel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val range = ranges.firstOrNull { it.index == sel } ?: ranges.first()
    var lz4 by rememberSaveable { mutableStateOf(true) }
    var full by rememberSaveable { mutableStateOf(false) }
    var dest by remember { mutableStateOf<Uri?>(null) }
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    var result by remember { mutableStateOf<JSONObject?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var verifyResult by remember { mutableStateOf<JSONObject?>(null) }
    var verifyError by remember { mutableStateOf<String?>(null) }
    var verifying by remember { mutableStateOf(false) }
    var userCancel by remember { mutableStateOf(false) }
    var cancelledNote by remember { mutableStateOf(false) }
    var inUse by remember { mutableStateOf(false) }
    LaunchedEffectUi(range.index, c.table) { inUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, range) } }

    val pickDest = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri: Uri? ->
        if (uri != null) { dest = uri; result = null; error = null; verifyResult = null; verifyError = null; cancelledNote = false }
    }

    // ---- 復元の状態
    var rsel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val rrange = ranges.firstOrNull { it.index == rsel } ?: ranges.first()
    var src by remember { mutableStateOf<Uri?>(null) }
    var srcInfo by remember { mutableStateOf<JSONObject?>(null) }
    var srcError by remember { mutableStateOf<String?>(null) }
    var rplan by remember { mutableStateOf<JSONObject?>(null) }
    var rplanError by remember { mutableStateOf<String?>(null) }
    var rplanKey by remember { mutableStateOf<List<Any?>?>(null) }
    var rjob by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { rjob }
    val rprogress = jobProgress(rjob)
    var rresult by remember { mutableStateOf<JSONObject?>(null) }
    var rerror by remember { mutableStateOf<String?>(null) }
    var confirm by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var rInUse by remember { mutableStateOf(false) }
    LaunchedEffectUi(rrange.index, c.table) { rInUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, rrange) } }
    val running = job != null || rjob != null || verifying

    val pickSrc = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) {
            src = uri; srcInfo = null; srcError = null; rplan = null; rplanError = null; rresult = null; rerror = null
            scope.launch {
                val o = withContext(Dispatchers.IO) { c.a.imageInspect(uri, false) }
                srcInfo = o.value; srcError = o.error
            }
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ======================== バックアップ
        ToolHeader(false, stringResource(R.string.dt_bak_header), stringResource(R.string.dt_bak_header_text))
        SectionCard(stringResource(R.string.dt_bak_source)) {
            RangePicker(
                ranges.map { if (it.whole) it.copy(label = stringResource(R.string.dt_range_whole)) else it }, sel,
                onSelect = { sel = it.index; result = null; error = null }, enabled = { !running },
            )
            if (inUse) Banner(Notice(Severity.Info, stringResource(R.string.dt_bak_mounted)), Modifier.testTag("bak_mounted_note"))
        }
        SectionCard(stringResource(R.string.dt_bak_options)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(R.string.dt_bak_lz4), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(R.string.dt_bak_lz4_sub), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = lz4, onCheckedChange = { lz4 = it }, enabled = !running, modifier = Modifier.testTag("bak_lz4"))
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(R.string.dt_bak_full), style = MaterialTheme.typography.titleSmall)
                    Text(stringResource(R.string.dt_bak_full_sub), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(checked = full, onCheckedChange = { full = it }, enabled = !running, modifier = Modifier.testTag("bak_full"))
            }
        }
        SectionCard(stringResource(R.string.dt_bak_dest)) {
            OutlinedButton(onClick = { pickDest.launch("usbmem-backup.img") }, enabled = !running, modifier = Modifier.fillMaxWidth().testTag("bak_pick_dest")) { Text(stringResource(R.string.dt_bak_pick_dest)) }
            dest?.let { KeyValue(stringResource(R.string.dt_bak_dest_file), displayName(it), Modifier.testTag("bak_dest_name")) }
            Text(stringResource(R.string.dt_bak_dest_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        if (job == null) {
            Button(
                enabled = dest != null && !running, modifier = Modifier.fillMaxWidth().testTag("bak_run"),
                onClick = {
                    val d = dest ?: return@Button
                    result = null; error = null; verifyResult = null; verifyError = null; userCancel = false; cancelledNote = false
                    scope.launchJob(
                        newJob = { c.a.newRecoveryJob() }, onJob = { job = it },
                        work = { j -> c.a.backupRun(c.target, range, d, lz4, full, j) },
                        done = { o -> result = o.value; error = o.error; cancelledNote = userCancel && o.value?.optBoolean("ok") != true },
                    )
                },
            ) { Text(stringResource(R.string.dt_bak_run)) }
        } else {
            ProgressRow(progress)
            OutlinedButton(onClick = { userCancel = true; job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("bak_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
        }
        error?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_bak_failed), it), Modifier.testTag("bak_error"), scrollIntoView = true) }
        result?.let { r ->
            Column(Modifier.testTag("bak_result"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (r.optBoolean("ok")) {
                    Banner(
                        Notice(Severity.Success, stringResource(R.string.dt_bak_done), stringResource(R.string.dt_bak_done_detail, r.optString("fs").uppercase(), r.long("usedBlocks"), r.long("totalBlocks"), formatBytes(r.long("imageBytes")))),
                        Modifier.testTag("bak_done"), scrollIntoView = true,
                    )
                    OutlinedButton(
                        enabled = !running && dest != null, modifier = Modifier.fillMaxWidth().testTag("bak_verify"),
                        onClick = {
                            val d = dest ?: return@OutlinedButton
                            verifying = true; verifyResult = null; verifyError = null
                            scope.launch {
                                val o = withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { c.a.imageInspect(d, true) }
                                verifyResult = o.value; verifyError = o.error; verifying = false
                            }
                        },
                    ) { Text(stringResource(R.string.dt_bak_verify)) }
                    if (verifying) ProgressRow(null)
                    verifyResult?.let { v ->
                        if (v.optBoolean("ok", true) && v.optBoolean("verified", true)) Banner(Notice(Severity.Success, stringResource(R.string.dt_bak_verified)), Modifier.testTag("bak_verified"), scrollIntoView = true)
                        else Banner(Notice(Severity.Error, stringResource(R.string.dt_bak_verify_failed), v.optString("message")), Modifier.testTag("bak_verify_failed"), scrollIntoView = true)
                    }
                    verifyError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_bak_verify_failed), it), Modifier.testTag("bak_verify_failed"), scrollIntoView = true) }
                } else {
                    Banner(
                        Notice(if (cancelledNote) Severity.Warning else Severity.Error, if (cancelledNote) stringResource(R.string.dt_bak_cancelled) else diskCode(r.optString("code")), r.optString("message")),
                        Modifier.testTag(if (cancelledNote) "bak_cancelled" else "bak_failed"), scrollIntoView = true,
                    )
                    if (cancelledNote) Text(stringResource(R.string.dt_bak_cancelled_note), style = MaterialTheme.typography.bodySmall)
                }
            }
        }

        // ======================== 復元 (書き込みます)
        DangerCard(stringResource(R.string.dt_res_title), subtitle = stringResource(R.string.dt_res_sub)) {
            if (!c.writeAllowed) Banner(
                Notice(Severity.Warning, stringResource(R.string.dt_need_write_title), stringResource(if (c.target.kind == DiskTargetKind.Usb) R.string.dt_need_write_usb else R.string.dt_need_write_image)),
                Modifier.testTag("res_need_write"),
            )
            OutlinedButton(onClick = { pickSrc.launch(arrayOf("*/*")) }, enabled = !running, modifier = Modifier.fillMaxWidth().testTag("res_pick_src")) { Text(stringResource(R.string.dt_res_pick_src)) }
            src?.let { KeyValue(stringResource(R.string.dt_res_src_file), displayName(it), Modifier.testTag("res_src_name")) }
            srcError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_res_src_bad), it), Modifier.testTag("res_src_error")) }
            srcInfo?.let { i ->
                if (!i.optBoolean("ok", true)) Banner(Notice(Severity.Error, stringResource(R.string.dt_res_src_bad), i.optString("message")), Modifier.testTag("res_src_error"))
            }

            Text(stringResource(R.string.dt_res_target), style = MaterialTheme.typography.titleSmall)
            RangePicker(
                ranges.map { if (it.whole) it.copy(label = stringResource(R.string.dt_range_whole)) else it }, rsel,
                onSelect = { rsel = it.index; rplan = null; rplanError = null; rresult = null; rerror = null }, enabled = { !running },
            )
            if (rInUse) Banner(Notice(Severity.Error, stringResource(R.string.dt_in_use_title), stringResource(R.string.dt_in_use_restore)), Modifier.testTag("res_in_use"))

            DangerOutlinedButton(
                stringResource(R.string.dt_plan_button), enabled = src != null && srcInfo?.optBoolean("ok", true) != false && !running && !busy, modifier = Modifier.testTag("res_plan"),
                onClick = {
                    val s = src ?: return@DangerOutlinedButton
                    rplan = null; rplanError = null; rresult = null; rerror = null; busy = true
                    val key = listOf(s, rrange.index)
                    scope.launch {
                        val o = withContext(Dispatchers.IO) { c.a.restorePlan(c.target, rrange, s) }
                        rplan = o.value; rplanError = o.error; rplanKey = key; busy = false
                    }
                },
            )
            if (busy) ProgressRow(null)
            rplanError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_plan_failed), it), Modifier.testTag("res_plan_error")) }
            val p = rplan
            val valid = p != null && rplanKey == listOf<Any?>(src, rrange.index)
            if (p != null && valid) RestorePlanCard(p, srcInfo, rrange, c, rInUse, running, onApply = { confirm = true })

            if (rjob != null) {
                ProgressRow(rprogress)
                OutlinedButton(onClick = { rjob?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("res_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
            }
            rerror?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("res_apply_error"), scrollIntoView = true) }
            rresult?.let { r ->
                if (r.optBoolean("ok")) Banner(
                    Notice(if (r.optBoolean("verified")) Severity.Success else Severity.Warning, stringResource(if (r.optBoolean("verified")) R.string.dt_res_done_verified else R.string.dt_res_done), stringResource(R.string.dt_res_done_detail, r.long("blocksWritten"))),
                    Modifier.testTag("res_done"), scrollIntoView = true,
                ) else Banner(
                    Notice(Severity.Error, diskCode(r.optString("code")), r.optString("message") + if (r.optBoolean("targetTouched")) "\n" + stringResource(R.string.dt_res_touched) else ""),
                    Modifier.testTag("res_failed"), scrollIntoView = true,
                )
            }
        }

        if (confirm && rplan != null) {
            val p = rplan!!
            val s = src
            TypedConfirmDialog(
                title = stringResource(R.string.dt_res_confirm_title),
                warning = stringResource(R.string.dt_res_confirm_warn),
                subject = listOf(
                    stringResource(R.string.dt_subject_device) to c.target.title,
                    stringResource(R.string.dt_subject_range) to (if (rrange.whole) stringResource(R.string.dt_range_whole) else rrange.label) + " (${fsText(rrange.fs)})",
                    stringResource(R.string.dt_subject_size) to formatBytes(rrange.size),
                    stringResource(R.string.dt_res_src_file) to (s?.let { displayName(it) } ?: ""),
                    stringResource(R.string.dt_subject_new_fs) to p.optString("fs").uppercase(),
                ),
                phrase = p.optString("confirmPhrase"), confirmLabel = stringResource(R.string.dt_res_do),
                onCancel = { confirm = false },
                onConfirm = { typed ->
                    confirm = false
                    val sUri = s ?: return@TypedConfirmDialog
                    rresult = null; rerror = null
                    scope.launchJob(
                        newJob = { c.a.newRecoveryJob() }, onJob = { rjob = it },
                        work = { j -> c.a.restoreApply(c.target, rrange, sUri, p.optString("confirmPhrase"), typed, j) },
                        done = { o -> rresult = o.value; rerror = o.error; rplan = null; c.reload() },
                    )
                },
            )
        }
    }
}

/** 復元の計画。取り違え防止のため、画像 (復元元) と書き込み先の FS・容量・ラベルを左右に並べる。 */
@Composable
private fun RestorePlanCard(p: JSONObject, info: JSONObject?, target: DiskRange, c: ToolContext, inUse: Boolean, running: Boolean, onApply: () -> Unit) {
    val ok = p.optBoolean("ok")
    val imgFs = p.optString("fs")
    val tgtFs = p.optString("targetExistingFs")
    val imgSize = info?.long("deviceSize") ?: 0L
    val tgtSize = p.long("targetSize")
    val fsMismatch = tgtFs.isNotBlank() && tgtFs != "unknown" && imgFs.isNotBlank() && tgtFs != imgFs
    val sizeMismatch = imgSize > 0 && tgtSize > 0 && imgSize != tgtSize
    Column(Modifier.testTag("res_plan_card"), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(stringResource(R.string.dt_plan_title), style = MaterialTheme.typography.titleSmall)
        if (!ok) Banner(Notice(Severity.Error, diskCode(p.optString("code")), p.optString("message")), Modifier.testTag("res_plan_bad"))
        // 左右の比較 (画像 / 書き込み先)
        Row(Modifier.fillMaxWidth().testTag("res_compare"), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(stringResource(R.string.dt_res_col_image), style = MaterialTheme.typography.labelLarge)
                KeyValue(stringResource(R.string.dt_col_fs), fsText(imgFs), Modifier.testTag("res_cmp_img_fs"))
                KeyValue(stringResource(R.string.dt_col_size), if (imgSize > 0) formatBytes(imgSize) else "?", Modifier.testTag("res_cmp_img_size"))
                KeyValue(stringResource(R.string.dt_res_used), formatBytes(p.long("bytesToWrite")))
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(stringResource(R.string.dt_res_col_target), style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.error)
                KeyValue(stringResource(R.string.dt_col_fs), fsText(tgtFs), Modifier.testTag("res_cmp_tgt_fs"))
                KeyValue(stringResource(R.string.dt_col_size), formatBytes(if (tgtSize > 0) tgtSize else target.size), Modifier.testTag("res_cmp_tgt_size"))
                KeyValue(stringResource(R.string.dt_res_label), target.fsLabel.ifBlank { "-" }, Modifier.testTag("res_cmp_tgt_label"))
            }
        }
        if (fsMismatch) Banner(Notice(Severity.Warning, stringResource(R.string.dt_res_fs_mismatch, fsText(imgFs), fsText(tgtFs))), Modifier.testTag("res_fs_mismatch"))
        if (sizeMismatch) Banner(Notice(Severity.Warning, stringResource(R.string.dt_res_size_mismatch, formatBytes(imgSize), formatBytes(tgtSize))), Modifier.testTag("res_size_mismatch"))
        diagnostics(p.optJSONArray("warnings")).forEach { (sev, m) -> DiagLine(sev, m) }
        KeyValue(stringResource(R.string.dt_plan_confirm_phrase), p.optString("confirmPhrase"))
        DangerButton(stringResource(R.string.dt_res_apply_button), onClick = onApply, modifier = Modifier.testTag("res_apply"), enabled = ok && c.writeAllowed && !inUse && !running)
    }
}
