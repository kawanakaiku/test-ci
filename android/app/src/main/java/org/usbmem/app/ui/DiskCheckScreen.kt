// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
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
import org.usbmem.app.DiskCheck
import org.usbmem.app.FsCheckResult
import org.usbmem.app.FsSeverity
import org.usbmem.app.R

/** 検査 (fsck 相当・読み取り専用)。書き込まない・修復しないことを、見出しと本文で明示する。docs/disk-tools.md 1 章 */
@Composable
fun DiskCheckScreen(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { rangesOf(c.table, "") }
    var sel by rememberSaveable { mutableIntStateOf(initialRangeIndex(ranges, c.target.preselectStart, c.initialRange)) }
    val range = ranges.firstOrNull { it.index == sel } ?: ranges.first()
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    var result by remember { mutableStateOf<FsCheckResult?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var cancelled by remember { mutableStateOf(false) }
    var userCancel by remember { mutableStateOf(false) }
    var mounted by remember { mutableStateOf(false) }
    LaunchedEffectUi(range.index, c.table) { mounted = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { c.a.inUse(c.target, range) } }
    val running = job != null

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        ToolHeader(false, stringResource(R.string.dt_check_header), stringResource(R.string.dt_check_header_text))

        SectionCard(stringResource(R.string.dt_pick_range)) {
            RangePicker(ranges.map { if (it.whole) it.copy(label = stringResource(R.string.dt_range_whole)) else it }, sel, onSelect = { sel = it.index; result = null; error = null; cancelled = false }, enabled = { !running }) { r ->
                if (r.fs.isNotBlank() && r.fs != "unknown" && !DiskCheck.supported(r.fs) && !org.usbmem.app.CryptoMount.isCryptoFs(r.fs)) stringResource(R.string.dt_check_fs_unsupported, fsText(r.fs)) else null
            }
            if (mounted) Banner(Notice(Severity.Info, stringResource(R.string.dt_check_mounted)), Modifier.testTag("check_mounted_note"))
        }

        if (job == null) {
            Button(
                onClick = {
                    result = null; error = null; cancelled = false; userCancel = false
                    scope.launchJob(
                        newJob = { c.a.newCheckJob() }, onJob = { job = it },
                        work = { j -> c.a.check(c.target, range, j) },
                        done = { o -> result = o.value; error = o.error; cancelled = userCancel },
                    )
                },
                modifier = Modifier.fillMaxWidth().testTag("check_run"),
            ) { Text(stringResource(R.string.dt_check_run)) }
        } else {
            ProgressRow(progress)
            OutlinedButton(onClick = { userCancel = true; job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("check_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
        }

        error?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_check_failed), it), Modifier.testTag("check_error"), scrollIntoView = true) }
        result?.let { r -> CheckResult(r, cancelled) }

        // 検査しないもの (限界)。結果が出る前でも、選んだ FS の分を見せる (何を信用してはいけないか)
        val fsForLimits = result?.fs ?: range.fs
        SectionCard(stringResource(R.string.dt_check_limits_title), subtitle = stringResource(R.string.dt_check_limits_sub), modifier = Modifier.testTag("check_limits")) {
            Text(stringResource(limitsText(fsForLimits)), style = MaterialTheme.typography.bodyMedium)
        }
    }
}

private fun limitsText(fs: String): Int = when {
    fs.startsWith("ext") -> R.string.dt_check_limits_ext
    fs == "ntfs" -> R.string.dt_check_limits_ntfs
    fs == "exfat" -> R.string.dt_check_limits_exfat
    fs.startsWith("fat") -> R.string.dt_check_limits_fat
    else -> R.string.dt_check_limits_other
}

@Composable
private fun CheckResult(r: FsCheckResult, cancelled: Boolean) {
    val (sev, titleRes) = when {
        cancelled -> Severity.Warning to R.string.dt_check_v_cancelled
        r.verdict == "clean" -> Severity.Success to R.string.dt_check_v_clean
        r.verdict == "warnings" -> Severity.Warning to R.string.dt_check_v_warnings
        r.verdict == "errors" -> Severity.Error to R.string.dt_check_v_errors
        r.verdict == "unsupported" -> Severity.Info to R.string.dt_check_v_unsupported
        else -> Severity.Warning to R.string.dt_check_v_incomplete
    }
    Column(Modifier.testTag("check_result"), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Banner(Notice(sev, stringResource(titleRes), stringResource(R.string.dt_check_fs_line, fsText(r.fs))), Modifier.testTag("check_verdict_${r.verdict}"), scrollIntoView = true)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Chip(stringResource(R.string.dt_check_count_errors, r.errors), severity = if (r.errors > 0) Severity.Error else Severity.Info, modifier = Modifier.testTag("check_count_errors"))
            Chip(stringResource(R.string.dt_check_count_warnings, r.warnings), severity = if (r.warnings > 0) Severity.Warning else Severity.Info, modifier = Modifier.testTag("check_count_warnings"))
            Chip(stringResource(R.string.dt_check_count_infos, r.infos))
        }
        if (r.needsJournalRecovery) Banner(Notice(Severity.Warning, stringResource(R.string.dt_check_journal)))
        else if (r.unclean) Banner(Notice(Severity.Warning, stringResource(R.string.dt_check_unclean)))
        if (r.findings.isNotEmpty()) {
            SectionCard(stringResource(R.string.dt_check_findings, r.findings.size)) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.testTag("check_findings")) {
                    r.findings.take(100).forEach { f ->
                        val sev2 = when (f.severity) { FsSeverity.Error -> Severity.Error; FsSeverity.Warning -> Severity.Warning; FsSeverity.Info -> Severity.Info }
                        DiagLine(sev2, buildString {
                            append(f.code); append(": "); append(f.message)
                            f.obj?.let { append(" ["); append(it); append("]") }
                            f.offset?.let { append(" @"); append(it) }
                        })
                    }
                    if (r.findings.size > 100 || r.findingsTruncated) Text(stringResource(R.string.dt_check_more, (r.findings.size - 100).coerceAtLeast(0)), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        Text(stringResource(R.string.dt_check_no_write_done), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("check_no_write"))
    }
}
