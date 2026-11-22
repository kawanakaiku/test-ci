// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.usbmem.app.R
import org.usbmem.app.formatBytes

private fun displayNameR(u: Uri): String = u.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':') ?: u.toString()

/**
 * 救出コピー (ddrescue 相当。docs/recovery-backup.md)。読み取り元 (**読み取り専用**) → 出力イメージのファイル。
 * 進捗ファイル (mapfile。ddrescue 互換) を残すので、取り消し・自動停止・抜けたあとも、同じ指定でもう一度実行すれば続きから行える。
 * 開始前に必ず `rescue_preflight` の結果 (出力の大きさ・進捗ファイルの状態・警告) を見せ、拒否されたら始められない。
 */
@Composable
fun DiskRescueScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { rangesOf(c.table, "") }
    var sel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val range = ranges.firstOrNull { it.index == sel } ?: ranges.first()
    var out by remember { mutableStateOf<Uri?>(null) }
    val isUsb = c.target.kind == DiskTargetKind.Usb
    var retries by rememberSaveable { mutableIntStateOf(0) }
    var noTrim by rememberSaveable { mutableStateOf(false) }
    var noScrape by rememberSaveable { mutableStateOf(false) }
    var pauseMs by rememberSaveable { mutableStateOf("0") }
    var maxErr by rememberSaveable { mutableStateOf(if (isUsb) "64" else "0") }
    var noProg by rememberSaveable { mutableStateOf(if (isUsb) "120" else "0") }
    val spec = RescueSpec(
        retries = retries, noTrim = noTrim, noScrape = noScrape, pauseBetweenReadsMs = pauseMs.toIntOrNull() ?: 0,
        maxConsecutiveErrors = maxErr.toIntOrNull() ?: 0, noProgressTimeoutSec = noProg.toIntOrNull() ?: 0, clusterSectors = 128,
    )
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    var paused by remember { mutableStateOf(false) }
    var pre by remember { mutableStateOf<JSONObject?>(null) }
    var preError by remember { mutableStateOf<String?>(null) }
    var preBusy by remember { mutableStateOf(false) }
    var tick by remember { mutableIntStateOf(0) }
    var result by remember { mutableStateOf<JSONObject?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    val running = job != null

    // 開始前の検査: 出力・範囲・設定が変わるたびに取り直す (走っている間は取り直さない)
    val preKey = listOf(range.index, out, spec, tick)
    LaunchedEffectUi(preKey, running) {
        val o = out
        if (o == null || running) { if (o == null) { pre = null; preError = null }; return@LaunchedEffectUi }
        preBusy = true
        delay(250)
        val r = withContext(Dispatchers.IO) { c.a.rescuePreflight(c.target, range, o, spec) }
        pre = r.value; preError = r.error; preBusy = false
    }

    val pickNew = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri: Uri? ->
        if (uri != null) { out = uri; result = null; error = null }
    }
    val pickExisting = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) { out = uri; result = null; error = null }
    }
    val preOk = pre?.optBoolean("ok") == true && !preBusy

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        ToolHeader(false, stringResource(R.string.dt_rescue_header), stringResource(R.string.dt_rescue_header_text))

        SectionCard(stringResource(R.string.dt_rescue_source)) {
            RangePicker(
                ranges.map { if (it.whole) it.copy(label = stringResource(R.string.dt_range_whole)) else it }, sel,
                onSelect = { sel = it.index; result = null; error = null }, enabled = { !running },
            )
        }

        SectionCard(stringResource(R.string.dt_rescue_out), subtitle = stringResource(R.string.dt_rescue_out_sub)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = { pickNew.launch("usbmem-rescue.img") }, enabled = !running, modifier = Modifier.weight(1f).testTag("rescue_pick_out")) { Text(stringResource(R.string.dt_rescue_pick_new)) }
                OutlinedButton(onClick = { pickExisting.launch(arrayOf("*/*")) }, enabled = !running, modifier = Modifier.weight(1f).testTag("rescue_pick_existing")) { Text(stringResource(R.string.dt_rescue_pick_existing)) }
            }
            out?.let { KeyValue(stringResource(R.string.dt_rescue_out_file), displayNameR(it), Modifier.testTag("rescue_out_name")) }
            Text(stringResource(R.string.dt_rescue_map_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        SectionCard(stringResource(R.string.dt_rescue_options)) {
            Text(stringResource(R.string.dt_rescue_retries), style = MaterialTheme.typography.titleSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(0, 1, 3).forEach { n ->
                    FilterChip(selected = retries == n, onClick = { retries = n }, enabled = !running, label = { Text(n.toString()) }, modifier = Modifier.testTag("rescue_retries_$n"))
                }
            }
            SwitchRow(stringResource(R.string.dt_rescue_no_trim), stringResource(R.string.dt_rescue_no_trim_sub), noTrim, !running, "rescue_no_trim") { noTrim = it }
            SwitchRow(stringResource(R.string.dt_rescue_no_scrape), stringResource(R.string.dt_rescue_no_scrape_sub), noScrape, !running, "rescue_no_scrape") { noScrape = it }
            NumField(stringResource(R.string.dt_rescue_pause), stringResource(R.string.dt_rescue_pause_sub), pauseMs, !running, "rescue_pause") { pauseMs = it }
            Text(stringResource(R.string.dt_rescue_valve), style = MaterialTheme.typography.titleSmall)
            Text(stringResource(R.string.dt_rescue_valve_sub), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            NumField(stringResource(R.string.dt_rescue_max_err), null, maxErr, !running, "rescue_max_err") { maxErr = it }
            NumField(stringResource(R.string.dt_rescue_no_progress), null, noProg, !running, "rescue_no_progress") { noProg = it }
        }

        // ---- 開始前の検査 (rescue_preflight)
        if (out != null) PreflightCard(pre, preError, preBusy, running = running, onResetMap = {
            val o = out
            if (o != null) scope.launch { withContext(Dispatchers.IO) { c.a.rescueResetMap(c.target, range, o) }; tick++ }
        })

        if (job == null) {
            Button(
                enabled = out != null && preOk, modifier = Modifier.fillMaxWidth().testTag("rescue_run"),
                onClick = {
                    val o = out ?: return@Button
                    result = null; error = null; paused = false
                    scope.launchJob(
                        newJob = { c.a.newRecoveryJob() }, onJob = { job = it },
                        work = { j -> c.a.rescueRun(c.target, range, o, spec, j) },
                        done = { r -> result = r.value; error = r.error; tick++ },
                    )
                },
            ) { Text(stringResource(if (pre?.optString("mapfile") == "resume") R.string.dt_rescue_resume else R.string.dt_rescue_run)) }
        } else {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = { paused = !paused; job?.pause(paused) }, modifier = Modifier.weight(1f).testTag("rescue_pause_btn")) { Text(stringResource(if (paused) R.string.dt_rescue_continue else R.string.dt_rescue_pause_btn)) }
                OutlinedButton(onClick = { job?.cancel() }, modifier = Modifier.weight(1f).testTag("rescue_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
            }
            ProgressRow(progress?.let { if (it.total > 0) it else null }, Modifier)
            progress?.json?.let { RescueBand(it, "rescue_band") }
            progress?.json?.let { RescueStats(it) }
        }

        error?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_rescue_failed), it), Modifier.testTag("rescue_error"), scrollIntoView = true) }
        result?.let { r -> RescueResult(r) }
    }
}

@Composable
private fun SwitchRow(title: String, sub: String, checked: Boolean, enabled: Boolean, tag: String, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Text(sub, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onChange, enabled = enabled, modifier = Modifier.testTag(tag))
    }
}

@Composable
private fun NumField(label: String, sub: String?, value: String, enabled: Boolean, tag: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value, onValueChange = { v -> onChange(v.filter { it.isDigit() }.take(7)) }, singleLine = true, enabled = enabled,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        label = { Text(label) }, supportingText = sub?.let { { Text(it) } }, modifier = Modifier.fillMaxWidth().testTag(tag),
    )
}

@Composable
private fun PreflightCard(pre: JSONObject?, error: String?, busy: Boolean, onResetMap: () -> Unit, running: Boolean) {
    SectionCard(stringResource(R.string.dt_rescue_pre_title), subtitle = stringResource(R.string.dt_rescue_pre_sub), modifier = Modifier.testTag("rescue_preflight")) {
        if (busy && pre == null) ProgressRow(null)
        error?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_rescue_pre_failed), it), Modifier.testTag("rescue_pre_error")) }
        val p = pre ?: return@SectionCard
        val state = p.optString("mapfile")
        val ok = p.optBoolean("ok")
        if (ok) Banner(Notice(Severity.Success, stringResource(R.string.dt_rescue_pre_ok)), Modifier.testTag("rescue_pre_ok"))
        else Banner(Notice(Severity.Error, stringResource(R.string.dt_rescue_pre_refused), diskCode(p.optString("code")).let { c -> if (p.optString("message").isBlank() || p.optString("message") == c) c else c + "\n" + p.optString("message") }), Modifier.testTag("rescue_pre_refused"))
        KeyValue(stringResource(R.string.dt_rescue_pre_in), formatBytes(p.long("inputSize")))
        KeyValue(stringResource(R.string.dt_rescue_pre_out), formatBytes(p.long("outputSize")))
        KeyValue(stringResource(R.string.dt_rescue_pre_map), stringResource(when (state) {
            "resume" -> R.string.dt_rescue_map_resume
            "corrupt" -> R.string.dt_rescue_map_corrupt
            "unreadable" -> R.string.dt_rescue_map_unreadable
            else -> R.string.dt_rescue_map_new
        }), Modifier.testTag("rescue_pre_map"))
        if (state == "resume") {
            RescueBand(p, "rescue_pre_band")
            RescueStats(p)
        }
        val warns = p.optJSONArray("warnings")
        if (warns != null) for (i in 0 until warns.length()) {
            val w = warns.optString(i)
            val text = when (w) {
                "extend" -> stringResource(R.string.dt_rescue_warn_extend)
                "output-empty-but-map" -> stringResource(R.string.dt_rescue_warn_empty_output)
                else -> w
            }
            DiagLine(Severity.Warning, text)
        }
        if ((state == "resume" || state == "corrupt" || state == "unreadable") && !running) {
            OutlinedButton(onClick = onResetMap, modifier = Modifier.fillMaxWidth().testTag("rescue_reset_map")) { Text(stringResource(R.string.dt_rescue_reset_map)) }
        }
    }
}

/** 救出の進み具合の帯 (救出済み / 読めなかった / 未試行など。幅はバイト数に比例)。 */
@Composable
fun RescueBand(j: JSONObject, tag: String) {
    val cs = MaterialTheme.colorScheme
    val st = LocalStatusColors.current
    val finished = j.long("finished")
    val bad = j.long("bad")
    val nonTried = j.long("nonTried")
    val partial = j.long("nonTrimmed") + j.long("nonScraped")
    val total = (finished + bad + nonTried + partial).coerceAtLeast(1)
    val desc = stringResource(R.string.dt_rescue_band_desc, formatBytes(finished), formatBytes(bad), formatBytes(nonTried + partial))
    Column(Modifier.testTag(tag), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(
            Modifier.fillMaxWidth().height(22.dp).clip(MaterialTheme.shapes.small).background(cs.surfaceVariant).clearAndSetSemantics { contentDescription = desc },
        ) {
            listOf(finished to st.onSuccessContainer, partial to st.onWarningContainer, bad to cs.error, nonTried to cs.surfaceVariant).forEach { (v, col) ->
                if (v > 0) Box(Modifier.weight((v.toDouble() / total).toFloat().coerceAtLeast(0.005f)).height(22.dp).background(col))
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Legend(st.onSuccessContainer, stringResource(R.string.dt_rescue_rescued))
            Legend(cs.error, stringResource(R.string.dt_rescue_bad))
            Legend(st.onWarningContainer, stringResource(R.string.dt_rescue_partial))
            Legend(cs.surfaceVariant, stringResource(R.string.dt_rescue_untried))
        }
    }
}

@Composable
private fun Legend(color: androidx.compose.ui.graphics.Color, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Box(Modifier.size(10.dp).background(color))
        Text(text, style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
private fun RescueStats(j: JSONObject) {
    Column(Modifier.testTag("rescue_stats"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        KeyValue(stringResource(R.string.dt_rescue_rescued), formatBytes(j.long("finished")), Modifier.testTag("rescue_stat_finished"))
        KeyValue(stringResource(R.string.dt_rescue_bad), formatBytes(j.long("bad")), Modifier.testTag("rescue_stat_bad"))
        KeyValue(stringResource(R.string.dt_rescue_untried), formatBytes(j.long("nonTried") + j.long("nonTrimmed") + j.long("nonScraped")), Modifier.testTag("rescue_stat_untried"))
    }
}

@Composable
private fun RescueResult(r: JSONObject) {
    val stop = r.optString("stop")
    val ok = r.optBoolean("ok")
    val (sev, title) = when (stop) {
        "finished" -> (if (r.long("bad") > 0) Severity.Warning else Severity.Success) to (if (r.long("bad") > 0) R.string.dt_rescue_done_bad else R.string.dt_rescue_done)
        "cancelled" -> Severity.Warning to R.string.dt_rescue_stop_cancelled
        "safety-abort" -> Severity.Warning to R.string.dt_rescue_stop_safety
        "input-gone" -> Severity.Warning to R.string.dt_rescue_stop_gone
        else -> Severity.Error to R.string.dt_rescue_stop_error
    }
    val resumable = stop == "cancelled" || stop == "safety-abort" || stop == "input-gone"
    Column(Modifier.testTag("rescue_result"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Banner(
            Notice(sev, stringResource(title), r.optString("message").takeIf { it.isNotBlank() && !ok }),
            Modifier.testTag("rescue_stop_$stop"), scrollIntoView = true,
        )
        RescueBand(r, "rescue_result_band")
        RescueStats(r)
        if (resumable) Text(stringResource(R.string.dt_rescue_resumable), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("rescue_resumable"))
    }
}
