// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes
import java.util.concurrent.atomic.AtomicBoolean

/** ブートセクタの検査結果 1 件 (パーティション 1 つ分)。[json] = 検査結果、[error] = 検査できなかった理由。 */
private class BootRes(val range: DiskRange, val json: JSONObject?, val error: String?)

/** 検査するパーティション。パーティション表のないイメージ (全体が 1 つの FS) や、パーティションが 1 つも見えないディスクは、全体 1 つ。 */
private fun bootRanges(t: PartitionTable): List<DiskRange> {
    val all = rangesOf(t, "")
    val parts = all.filter { !it.whole }
    return if (t.wholeDeviceVolume || parts.isEmpty()) all.take(1) else parts
}

/**
 * ブートセクタを検査 / 修復 (docs/partition-scan.md 4 章)。パーティション編集の画面の口 ([DiskSlot.BootSector]) に差し込まれる。
 *
 * **検査 (読み取り専用・緑)**: パーティションごとにブートセクタ / スーパーブロックの異常を一覧する。書き込まない。
 * **修復 (書き込みます・赤)**: 検査が示した修復を 1 つ選ぶ → 計画 (書く場所と内容の予定。まだ書かない) → 確認文字列の入力 → 適用 (書く前に元の内容をアプリ内へ保存・書いたあと読み戻して再検査)。
 * NTFS の ntfsfix のような自動修復は使わない。直すのは計画に載ったブート構造だけで、ファイルシステムのデータには触れない。
 */
@Composable
fun BootSectorPanel(c: ToolContext) {
    val scope = rememberUiScope()
    val ranges = remember(c.table) { bootRanges(c.table) }
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    val cancelFlag = remember { AtomicBoolean(false) }
    var results by remember { mutableStateOf<List<BootRes>>(emptyList()) }
    var ran by remember { mutableStateOf(false) }
    var cancelled by remember { mutableStateOf(false) }

    var pickRange by remember { mutableStateOf<DiskRange?>(null) }
    var pickRepair by remember { mutableStateOf("") }
    var pickSource by remember { mutableStateOf(0L) }
    var pickLabel by remember { mutableStateOf("") }
    var plan by remember { mutableStateOf<JSONObject?>(null) }
    var planError by remember { mutableStateOf<String?>(null) }
    var planning by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf(false) }
    var applying by remember { mutableStateOf(false) }
    var done by remember { mutableStateOf<BootDone?>(null) }
    var applyError by remember { mutableStateOf<String?>(null) }
    var inUse by remember { mutableStateOf(false) }
    val running = job != null

    LaunchedEffectUi(pickRange?.index, c.table) {
        val pr = pickRange
        inUse = if (pr == null) false else withContext(Dispatchers.IO) { c.a.inUse(c.target, pr) }
    }

    fun resetPick() { pickRange = null; pickRepair = ""; plan = null; planError = null; confirm = false }

    fun pick(rg: DiskRange, id: String, src: Long, label: String) {
        pickRange = rg; pickRepair = id; pickSource = src; pickLabel = label
        plan = null; planError = null; done = null; applyError = null; planning = true
        scope.launch {
            val o = withContext(Dispatchers.IO) { c.a.bootPlan(c.target, rg, id, src) }
            plan = o.value; planError = o.error; planning = false
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ================= 検査 (読み取り専用)
        ToolHeader(false, stringResource(R.string.dt_boot_header), stringResource(R.string.dt_boot_header_text), tag = "boot_header_readonly")
        if (!running) {
            Button(
                onClick = {
                    results = emptyList(); ran = false; cancelled = false; cancelFlag.set(false); resetPick(); done = null; applyError = null
                    val rs = ranges
                    scope.launchJob(
                        newJob = { c.a.newScanJob() }, onJob = { job = it },
                        work = { j ->
                            val out = ArrayList<BootRes>()
                            for (rg in rs) {
                                if (cancelFlag.get()) break
                                val o = c.a.bootInspect(c.target, rg, j)
                                out.add(BootRes(rg, o.value, o.error))
                            }
                            out
                        },
                        done = { list -> results = list; ran = true; cancelled = cancelFlag.get() },
                    )
                },
                enabled = !applying && !planning, modifier = Modifier.fillMaxWidth().testTag("boot_run"),
            ) { Text(stringResource(R.string.dt_boot_run)) }
        } else {
            ProgressRow(bootProgressText(progress))
            OutlinedButton(onClick = { cancelFlag.set(true); job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("boot_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
        }
        Text(stringResource(R.string.dt_boot_no_auto), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("boot_no_auto"))

        if (ran) {
            Column(Modifier.testTag("boot_result"), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (cancelled) Banner(Notice(Severity.Warning, stringResource(R.string.dt_boot_cancelled)), Modifier.testTag("boot_cancelled"))
                if (results.isEmpty() && !cancelled) Text(stringResource(R.string.dt_boot_none), style = MaterialTheme.typography.bodyMedium)
                results.forEach { b -> BootResultCard(b) }
                Text(stringResource(R.string.dt_boot_read_only_done), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("boot_no_write"))
            }
        }

        // ================= 修復 (書き込みます)
        val withRepairs = results.filter { (it.json?.optJSONArray("repairs")?.length() ?: 0) > 0 }
        if (withRepairs.isNotEmpty() || done != null || applyError != null) {
            DangerCard(stringResource(R.string.dt_boot_repair_title), subtitle = stringResource(R.string.dt_boot_repair_sub), modifier = Modifier.testTag("boot_repair_card")) {
                if (!c.writeAllowed) Banner(
                    Notice(Severity.Warning, stringResource(R.string.dt_need_write_title), stringResource(if (c.target.kind == DiskTargetKind.Usb) R.string.dt_need_write_usb else R.string.dt_need_write_image)),
                    Modifier.testTag("boot_need_write"),
                )
                withRepairs.forEach { b ->
                    val reps = b.json?.optJSONArray("repairs")
                    if (reps != null) {
                        Text(rangeTitle(b.range), style = MaterialTheme.typography.titleSmall, modifier = Modifier.semantics { heading() })
                        for (k in 0 until reps.length()) {
                            val rp = reps.getJSONObject(k)
                            val id = rp.optString("id")
                            val src = rp.long("source")
                            val label = repairLabel(id, rp.optString("description"))
                            Text(label, style = MaterialTheme.typography.bodyMedium)
                            if (id.startsWith("ext-") && src > 0) Text(stringResource(R.string.dt_boot_ext_source, src), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            DangerOutlinedButton(
                                stringResource(R.string.dt_boot_plan_button), onClick = { pick(b.range, id, src, label) },
                                enabled = !running && !planning && !applying, modifier = Modifier.testTag("boot_plan_${b.range.index}_$id"),
                            )
                        }
                    }
                }
                if (planning) ProgressRow(null)
                planError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_plan_failed), it), Modifier.testTag("boot_plan_error")) }
                val pl = plan
                val pr = pickRange
                if (pl != null && pr != null) {
                    BootPlanView(pl, pickLabel, rangeTitle(pr))
                    if (inUse) Banner(Notice(Severity.Error, stringResource(R.string.dt_in_use_title), stringResource(R.string.dt_in_use_boot)), Modifier.testTag("boot_in_use"))
                    if (running) {
                        ProgressRow(bootProgressText(progress))
                        OutlinedButton(onClick = { job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("boot_apply_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
                    }
                    DangerButton(
                        stringResource(R.string.dt_boot_apply_button), onClick = { confirm = true }, modifier = Modifier.testTag("boot_apply"),
                        enabled = pl.optBoolean("ok") && c.writeAllowed && !inUse && !running && !applying,
                    )
                    if (!pl.optBoolean("ok")) Text(stringResource(R.string.dt_plan_not_ok), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
                }
                applyError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("boot_apply_error"), scrollIntoView = true) }
                done?.let { d ->
                    val res = d.result
                    if (res.ok) {
                        Banner(
                            Notice(if (res.verified) Severity.Success else Severity.Warning, stringResource(if (res.verified) R.string.dt_boot_done_verified else R.string.dt_boot_done), stringResource(R.string.dt_boot_done_detail, res.writesDone)),
                            Modifier.testTag("boot_done"), scrollIntoView = true,
                        )
                        d.backupName?.let { Text(stringResource(R.string.dt_backup_saved, it), style = MaterialTheme.typography.bodySmall) }
                    } else {
                        Banner(Notice(Severity.Error, diskCode(res.code), (if (res.rolledBack) stringResource(R.string.dt_rolled_back) + "\n" else "") + res.message), Modifier.testTag("boot_failed"), scrollIntoView = true)
                    }
                }
            }
        }
    }

    val pl = plan
    val pr = pickRange
    if (confirm && pl != null && pr != null) {
        val phrase = pl.optString("confirmPhrase")
        val fsName = results.firstOrNull { it.range.index == pr.index }?.json?.optString("fs") ?: ""
        TypedConfirmDialog(
            title = stringResource(R.string.dt_boot_confirm_title),
            warning = stringResource(R.string.dt_boot_confirm_warn),
            subject = listOf(
                stringResource(R.string.dt_subject_device) to c.target.title,
                stringResource(R.string.dt_subject_range) to rangeTitle(pr),
                stringResource(R.string.dt_subject_size) to formatBytes(pr.size),
                stringResource(R.string.dt_subject_old_fs) to fsText(fsName),
                stringResource(R.string.dt_boot_subject_repair) to pickLabel,
                stringResource(R.string.dt_plan_bytes) to formatBytes(pl.long("bytesToWrite")),
            ),
            phrase = phrase, confirmLabel = stringResource(R.string.dt_boot_apply_do),
            onCancel = { confirm = false },
            onConfirm = { typed ->
                confirm = false; done = null; applyError = null; applying = true
                val rg = pr
                val id = pickRepair
                val src = pickSource
                scope.launchJob(
                    newJob = { c.a.newScanJob() }, onJob = { job = it },
                    work = { j -> c.a.bootApply(c.target, rg, id, src, phrase, typed, j) },
                    done = { o ->
                        applying = false; done = o.value; applyError = o.error
                        val d = o.value
                        if (d != null && d.result.ok) {
                            val after = d.after
                            if (after != null) results = results.map { if (it.range.index == rg.index) BootRes(it.range, after, null) else it }
                            resetPick()
                        }
                        c.reload()
                    },
                )
            },
        )
    }
}

@Composable
private fun rangeTitle(r: DiskRange): String = if (r.whole) stringResource(R.string.dt_range_whole) else r.label

@Composable
private fun bootProgressText(p: OpProgress?): OpProgress? {
    if (p == null) return null
    val text = when (p.phase) {
        "backup" -> stringResource(R.string.dt_phase_backup)
        "write" -> stringResource(R.string.dt_phase_write)
        "verify" -> stringResource(R.string.dt_phase_verify)
        else -> p.phase
    }
    return p.copy(phase = text)
}

@Composable
private fun repairLabel(id: String, native: String): String = when (id) {
    "fat32-restore-primary-from-backup" -> stringResource(R.string.dt_boot_r_fat32_restore)
    "fat32-refresh-backup-from-primary" -> stringResource(R.string.dt_boot_r_fat32_refresh)
    "fat32-rebuild-fsinfo" -> stringResource(R.string.dt_boot_r_fat32_fsinfo)
    "ntfs-restore-primary-from-backup" -> stringResource(R.string.dt_boot_r_ntfs_restore)
    "ntfs-refresh-backup-from-primary" -> stringResource(R.string.dt_boot_r_ntfs_refresh)
    "exfat-restore-main-from-backup" -> stringResource(R.string.dt_boot_r_exfat_restore)
    "exfat-refresh-backup-from-main" -> stringResource(R.string.dt_boot_r_exfat_refresh)
    "exfat-recompute-checksums" -> stringResource(R.string.dt_boot_r_exfat_checksum)
    "ext-restore-superblock-from-backup" -> stringResource(R.string.dt_boot_r_ext_super)
    "ext-restore-gdt-from-backup" -> stringResource(R.string.dt_boot_r_ext_gdt)
    else -> native.ifBlank { id }
}

private fun sevOf(s: String): Severity = when (s.lowercase()) { "error" -> Severity.Error; "warning" -> Severity.Warning; else -> Severity.Info }

private fun findingLines(a: JSONArray?): List<Pair<Severity, String>> = if (a == null) emptyList() else List(a.length()) { i ->
    val o = a.getJSONObject(i)
    sevOf(o.optString("severity")) to (o.optString("code") + ": " + o.optString("message"))
}

@Composable
private fun BootResultCard(b: BootRes) {
    val idx = b.range.index
    var details by remember { mutableStateOf(false) }
    SectionCard(rangeTitle(b.range), subtitle = formatBytes(b.range.size), modifier = Modifier.testTag("boot_res_$idx")) {
        val j = b.json
        if (j == null) {
            Banner(Notice(Severity.Error, stringResource(R.string.dt_boot_failed), b.error), Modifier.testTag("boot_error_$idx"))
            return@SectionCard
        }
        val verdict = j.optString("verdict")
        val (sev, titleRes) = when (verdict) {
            "ok" -> Severity.Success to R.string.dt_boot_v_ok
            "warnings" -> Severity.Warning to R.string.dt_boot_v_warnings
            "repairable" -> Severity.Error to R.string.dt_boot_v_repairable
            "damaged" -> Severity.Error to R.string.dt_boot_v_damaged
            else -> Severity.Info to R.string.dt_boot_v_unrecognized
        }
        Banner(Notice(sev, stringResource(titleRes), stringResource(R.string.dt_check_fs_line, fsText(j.optString("fs")))), Modifier.testTag("boot_verdict_${idx}_$verdict"))
        val lines = findingLines(j.optJSONArray("findings"))
        if (lines.isNotEmpty()) {
            Column(Modifier.testTag("boot_findings_$idx"), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                lines.take(40).forEach { (s, t) -> DiagLine(s, t) }
                if (lines.size > 40) Text(stringResource(R.string.dt_check_more, lines.size - 40), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        val bpb = j.optJSONObject("proposedBpb")
        if (bpb != null && bpb.length() > 0) {
            Column(Modifier.testTag("boot_bpb_$idx"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(stringResource(R.string.dt_boot_bpb_title), style = MaterialTheme.typography.titleSmall)
                Text(stringResource(R.string.dt_boot_bpb_text), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                bpb.keys().asSequence().toList().forEach { k -> KeyValue(k, bpb.optString(k)) }
            }
        }
        val det = j.optJSONObject("details")
        if (det != null && det.length() > 0) {
            TextButton(onClick = { details = !details }, modifier = Modifier.testTag("boot_details_toggle_$idx")) { Text(stringResource(if (details) R.string.dt_boot_details_hide else R.string.dt_boot_details_show)) }
            if (details) Column(Modifier.testTag("boot_details_$idx"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                det.keys().asSequence().toList().forEach { k -> KeyValue(k, det.optString(k)) }
            }
        }
    }
}

@Composable
private fun BootPlanView(p: JSONObject, repairLabel: String, rangeLabel: String) {
    val problems = findingLines(p.optJSONArray("problems"))
    val changes = p.optJSONArray("changes")
    val writes = p.optJSONArray("writes")
    Column(Modifier.testTag("boot_plan_card"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.dt_plan_title), style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.dt_boot_plan_for, repairLabel, rangeLabel), style = MaterialTheme.typography.bodyMedium)
        problems.forEach { (s, t) -> DiagLine(s, t) }
        if (changes != null) for (i in 0 until changes.length()) Text("• " + changes.optString(i), style = MaterialTheme.typography.bodySmall)
        if (writes != null && writes.length() > 0) {
            Column(Modifier.testTag("boot_plan_writes"), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(stringResource(R.string.dt_boot_plan_writes, writes.length()), style = MaterialTheme.typography.labelLarge)
                for (i in 0 until writes.length()) {
                    val w = writes.getJSONObject(i)
                    Text("${w.optString("what")} · ${stringResource(R.string.dt_boot_plan_at, w.long("offset"), w.optInt("length"))}", style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        KeyValue(stringResource(R.string.dt_plan_confirm_phrase), p.optString("confirmPhrase"))
        KeyValue(stringResource(R.string.dt_plan_bytes), formatBytes(p.long("bytesToWrite")))
        Text(stringResource(R.string.dt_boot_backup_guide), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("boot_backup_guide"))
    }
}
