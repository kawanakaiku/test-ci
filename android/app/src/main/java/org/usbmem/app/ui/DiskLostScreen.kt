// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
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
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes
import java.security.SecureRandom

/**
 * 失われたパーティションを探す (TestDisk 相当。docs/partition-scan.md)。パーティション編集の画面の口 ([DiskSlot.LostPartitions]) に差し込まれる。
 *
 * 手順と見た目: **探索 (読み取り専用・緑)** → 候補の一覧 (開始・サイズ・種別・信頼度・既存の表との関係) → 復元案 (信頼度順。案ごとに変更後の表を帯グラフで見せる、読み取り専用)
 * → 案を選ぶと **適用 (書き込みます・赤)**: 計画 (変更の一覧・確認文字列) → 確認文字列の入力 → 適用。書くのはパーティション表だけで、書く前に表の控えをアプリ内へ保存する。
 */
@Composable
fun LostPartitionsPanel(c: ToolContext) {
    val scope = rememberUiScope()
    var full by rememberSaveable { mutableStateOf(false) }
    var job by remember { mutableStateOf<DiskOpJob?>(null) }
    CancelOnDispose { job }
    val progress = jobProgress(job)
    var report by remember { mutableStateOf<ScanReport?>(null) }
    var spec by remember { mutableStateOf<ScanSpec?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var selected by remember { mutableIntStateOf(-1) }
    var confirm by remember { mutableStateOf(false) }
    var applying by remember { mutableStateOf(false) }
    var applied by remember { mutableStateOf<PartitionDone?>(null) }
    var applyError by remember { mutableStateOf<String?>(null) }
    var inUse by remember { mutableStateOf(false) }
    val running = job != null
    LaunchedEffectUi(c.table) { inUse = withContext(Dispatchers.IO) { c.a.inUse(c.target, DiskRange(0, Long.MAX_VALUE / 2, "", "", 0)) } }

    fun newSeed(): Long {
        val v = SecureRandom().nextLong()
        return if (v == 0L) 1L else v
    }

    val r = report
    val sel = r?.proposals?.firstOrNull { it.json.optInt("index") == selected }
    val selPlan = sel?.plan

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ================= 探索 (読み取り専用)
        ToolHeader(false, stringResource(R.string.dt_lost_header), stringResource(R.string.dt_lost_header_text), tag = "lost_header_readonly")
        Text(stringResource(R.string.dt_lost_mode), style = MaterialTheme.typography.titleSmall)
        ChipFlow {
            FilterChip(selected = !full, onClick = { full = false }, enabled = !running, label = { Text(stringResource(R.string.dt_lost_mode_quick)) }, modifier = Modifier.testTag("lost_mode_quick"))
            FilterChip(selected = full, onClick = { full = true }, enabled = !running, label = { Text(stringResource(R.string.dt_lost_mode_full)) }, modifier = Modifier.testTag("lost_mode_full"))
        }
        Text(stringResource(if (full) R.string.dt_lost_mode_full_desc else R.string.dt_lost_mode_quick_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)

        if (!running) {
            Button(
                onClick = {
                    report = null; error = null; selected = -1; applied = null; applyError = null
                    val sp = ScanSpec(full, 0, newSeed())
                    spec = sp
                    scope.launchJob(
                        newJob = { c.a.newScanJob() }, onJob = { job = it },
                        work = { j -> c.a.scanRun(c.target, sp, j) },
                        done = { o -> report = o.value; error = o.error },
                    )
                },
                enabled = !applying, modifier = Modifier.fillMaxWidth().testTag("lost_run"),
            ) { Text(stringResource(R.string.dt_lost_run)) }
        } else {
            ProgressRow(scanProgressText(progress))
            OutlinedButton(onClick = { job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("lost_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
        }
        error?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_lost_failed), it), Modifier.testTag("lost_error"), scrollIntoView = true) }

        if (r != null) {
            ScanResultView(r)
            if (!r.cancelled) {
                Text(stringResource(R.string.dt_lost_props_title), style = MaterialTheme.typography.titleSmall, modifier = Modifier.semantics { heading() })
                if (r.proposals.isEmpty()) Text(stringResource(R.string.dt_lost_props_none), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("lost_props_none"))
                else Text(stringResource(R.string.dt_lost_props_sub), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                r.proposals.forEach { p ->
                    val idx = p.json.optInt("index")
                    ProposalCard(p, selected == idx, enabled = !running && !applying, onPick = { selected = if (selected == idx) -1 else idx; applied = null; applyError = null })
                }
            }
        }

        // ================= 適用 (書き込みます)
        if (sel != null && selPlan != null) {
            DangerCard(stringResource(R.string.dt_lost_apply_title), subtitle = stringResource(R.string.dt_lost_apply_sub), modifier = Modifier.testTag("lost_write_card")) {
                Text(stringResource(R.string.dt_lost_apply_what, proposalTitle(sel.json)), style = MaterialTheme.typography.titleSmall)
                if (!c.writeAllowed) Banner(
                    Notice(Severity.Warning, stringResource(R.string.dt_need_write_title), stringResource(if (c.target.kind == DiskTargetKind.Usb) R.string.dt_need_write_usb else R.string.dt_need_write_image)),
                    Modifier.testTag("lost_need_write"),
                )
                if (inUse) Banner(Notice(Severity.Error, stringResource(R.string.dt_in_use_title), stringResource(R.string.dt_in_use_partition)), Modifier.testTag("lost_in_use"))
                if (selPlan.destructive) Banner(Notice(Severity.Warning, stringResource(R.string.dt_pt_destructive)), Modifier.testTag("lost_destructive"))
                if (selPlan.requiresForce) Banner(Notice(Severity.Error, stringResource(R.string.dt_pt_force)))
                diagnostics(selPlan.json.optJSONArray("problems")).forEach { (sev, text) -> DiagLine(sev, text) }
                selPlan.changes.forEach { Text("• $it", style = MaterialTheme.typography.bodySmall) }
                KeyValue(stringResource(R.string.dt_plan_confirm_phrase), selPlan.confirmPhrase)
                Text(stringResource(R.string.dt_lost_backup_guide), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("lost_backup_guide"))
                if (running) {
                    ProgressRow(scanProgressText(progress))
                    OutlinedButton(onClick = { job?.cancel() }, modifier = Modifier.fillMaxWidth().testTag("lost_apply_cancel")) { Text(stringResource(R.string.dt_cancel_run)) }
                }
                DangerButton(
                    stringResource(R.string.dt_lost_apply_button), onClick = { confirm = true }, modifier = Modifier.testTag("lost_apply"),
                    enabled = selPlan.ok && c.writeAllowed && !inUse && !running,
                )
                if (!selPlan.ok) Text(stringResource(R.string.dt_plan_not_ok), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
        }
        applyError?.let { Banner(Notice(Severity.Error, stringResource(R.string.dt_apply_failed), it), Modifier.testTag("lost_apply_error"), scrollIntoView = true) }
        applied?.let { d ->
            val res = d.result
            if (res.ok) {
                Banner(
                    Notice(if (res.verified) Severity.Success else Severity.Warning, stringResource(if (res.verified) R.string.dt_lost_done_verified else R.string.dt_lost_done), stringResource(R.string.dt_pt_done_detail, res.sectorsWritten)),
                    Modifier.testTag("lost_done"), scrollIntoView = true,
                )
                d.backupName?.let { Text(stringResource(R.string.dt_backup_saved, it), style = MaterialTheme.typography.bodySmall) }
            } else {
                Banner(Notice(Severity.Error, diskCode(res.code), (if (res.rolledBack) stringResource(R.string.dt_rolled_back) + "\n" else "") + res.message), Modifier.testTag("lost_failed"), scrollIntoView = true)
            }
        }
    }

    val sp = spec
    if (confirm && sel != null && selPlan != null && sp != null) {
        val proposalId = sel.json.optString("id")
        val proposalIndex = sel.json.optInt("index")
        TypedConfirmDialog(
            title = stringResource(R.string.dt_lost_confirm_title),
            warning = stringResource(R.string.dt_lost_confirm_warn),
            subject = listOf(
                stringResource(R.string.dt_subject_device) to c.target.title,
                stringResource(R.string.dt_subject_table) to "${c.table.kind.uppercase()} · ${formatBytes(c.table.diskBytes)}",
                stringResource(R.string.dt_lost_subject_proposal) to proposalTitle(sel.json),
                stringResource(R.string.dt_subject_changes) to selPlan.changes.size.toString(),
                stringResource(R.string.dt_subject_sectors) to selPlan.sectorCount.toString(),
            ),
            phrase = selPlan.confirmPhrase, confirmLabel = stringResource(R.string.dt_pt_apply_do),
            onCancel = { confirm = false },
            onConfirm = { typed ->
                confirm = false; applied = null; applyError = null; applying = true
                val phrase = selPlan.confirmPhrase
                scope.launchJob(
                    newJob = { c.a.newScanJob() }, onJob = { job = it },
                    work = { j -> c.a.scanApply(c.target, sp, proposalId, proposalIndex, phrase, typed, j) },
                    done = { o ->
                        applying = false; applied = o.value; applyError = o.error
                        if (o.value?.result?.ok == true) { report = null; selected = -1 }
                        c.reload()
                    },
                )
            },
        )
    }
}

/** 探索の段階名を、利用者に見せる文言にした進捗 (段階名は table / scan / verify / backup / write)。 */
@Composable
private fun scanProgressText(p: OpProgress?): OpProgress? {
    if (p == null) return null
    val text = when (p.phase) {
        "table" -> stringResource(R.string.dt_phase_table)
        "scan" -> stringResource(R.string.dt_phase_scan)
        "verify" -> stringResource(R.string.dt_phase_verify)
        "backup" -> stringResource(R.string.dt_phase_backup)
        "write" -> stringResource(R.string.dt_phase_write)
        else -> p.phase
    }
    return p.copy(phase = text)
}

@Composable
private fun proposalTitle(j: JSONObject): String = when (j.optString("id")) {
    "gpt-copy-backup-to-primary" -> stringResource(R.string.dt_lost_p_gpt_backup_to_primary)
    "gpt-copy-primary-to-backup" -> stringResource(R.string.dt_lost_p_gpt_primary_to_backup)
    "gpt-move-backup-to-end" -> stringResource(R.string.dt_lost_p_gpt_move_backup)
    "gpt-rebuild-protective-mbr" -> stringResource(R.string.dt_lost_p_gpt_protective)
    "add-missing" -> stringResource(R.string.dt_lost_p_add_missing)
    "rebuild-gpt" -> stringResource(R.string.dt_lost_p_rebuild_gpt)
    "rebuild-mbr" -> stringResource(R.string.dt_lost_p_rebuild_mbr)
    else -> j.optString("title").ifBlank { j.optString("id") }
}

@Composable
private fun noteText(code: String): String = when (code) {
    "primary-damaged" -> stringResource(R.string.dt_lost_n_primary_damaged)
    "found-by-backup" -> stringResource(R.string.dt_lost_n_found_by_backup)
    "exceeds-device" -> stringResource(R.string.dt_lost_n_exceeds_device)
    "size-unknown" -> stringResource(R.string.dt_lost_n_size_unknown)
    "types-guessed" -> stringResource(R.string.dt_lost_n_types_guessed)
    "sizes-estimated" -> stringResource(R.string.dt_lost_n_sizes_estimated)
    "gpt-remnants" -> stringResource(R.string.dt_lost_n_gpt_remnants)
    "mbr-remnants" -> stringResource(R.string.dt_lost_n_mbr_remnants)
    "mbr-non-512-sector" -> stringResource(R.string.dt_lost_n_mbr_non512)
    else -> code
}

@Composable
private fun relationText(rel: String): String = when (rel) {
    "match" -> stringResource(R.string.dt_lost_rel_match)
    "overlap" -> stringResource(R.string.dt_lost_rel_overlap)
    "contained" -> stringResource(R.string.dt_lost_rel_contained)
    else -> stringResource(R.string.dt_lost_rel_missing)
}

private fun confidenceSeverity(c: Int): Severity = if (c >= 80) Severity.Success else if (c >= 50) Severity.Warning else Severity.Info

@Composable
private fun ScanResultView(r: ScanReport) {
    val res = r.result
    val cands = res.optJSONArray("candidates")
    val n = cands?.length() ?: 0
    var lost = 0
    for (i in 0 until n) if (cands?.getJSONObject(i)?.optString("relation") == "not-in-table") lost++
    val shown = minOf(n, 60)
    Column(Modifier.testTag("lost_result"), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (r.cancelled) {
            Banner(Notice(Severity.Warning, stringResource(R.string.dt_lost_cancelled), stringResource(R.string.dt_lost_cancelled_detail)), Modifier.testTag("lost_cancelled"), scrollIntoView = true)
        } else {
            Banner(
                Notice(if (lost > 0) Severity.Warning else Severity.Info, stringResource(R.string.dt_lost_found, n, lost), stringResource(R.string.dt_lost_stats, res.long("positionsProbed"), formatBytes(res.long("bytesRead")), res.long("readErrors"))),
                Modifier.testTag("lost_found"), scrollIntoView = true,
            )
        }
        val noFs = res.optJSONArray("tableEntriesWithoutFs")
        if (noFs != null && noFs.length() > 0) {
            val nums = (0 until noFs.length()).joinToString(", ") { "#" + noFs.optString(it) }
            Text(stringResource(R.string.dt_lost_table_no_fs, nums), style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("lost_table_no_fs"))
        }
        if (n == 0 && !r.cancelled) Text(stringResource(R.string.dt_lost_none), style = MaterialTheme.typography.bodyMedium, modifier = Modifier.testTag("lost_none"))
        for (i in 0 until shown) {
            val o = cands?.getJSONObject(i) ?: continue
            CandidateCard(o, i)
        }
        if (n > shown) Text(stringResource(R.string.dt_lost_more, n - shown), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(stringResource(R.string.dt_lost_read_only_done), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("lost_no_write"))
    }
}

@Composable
private fun CandidateCard(o: JSONObject, i: Int) {
    val cs = MaterialTheme.colorScheme
    val rel = o.optString("relation")
    val missing = rel == "not-in-table"
    val conf = o.optInt("confidence")
    val sizeText = formatBytes(o.long("sizeBytes")) + when {
        !o.optBoolean("sizeKnown", true) -> " " + stringResource(R.string.dt_lost_size_unknown)
        o.optBoolean("sizeMinimum") -> " " + stringResource(R.string.dt_lost_size_minimum)
        else -> ""
    }
    val notes = o.optJSONArray("notes")
    Surface(
        Modifier.fillMaxWidth().testTag("lost_cand_$i").semantics(mergeDescendants = true) { },
        shape = MaterialTheme.shapes.medium, color = cs.surfaceContainerHighest,
        border = BorderStroke(if (missing) 2.dp else 1.dp, if (missing) cs.tertiary else cs.outlineVariant),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            ChipFlow {
                Chip(fsText(o.optString("fs")))
                Chip(stringResource(R.string.dt_lost_confidence, conf), severity = confidenceSeverity(conf))
                Chip(relationText(rel), severity = if (missing) Severity.Warning else Severity.Info)
            }
            KeyValue(stringResource(R.string.dt_lost_start), "LBA ${o.long("startLba")} · ${formatBytes(o.long("startBytes"))}")
            KeyValue(stringResource(R.string.dt_col_size), sizeText)
            if (o.optInt("tableIndex") > 0 && rel != "not-in-table") KeyValue(stringResource(R.string.dt_lost_table_entry), "#${o.optInt("tableIndex")}")
            if (notes != null) for (k in 0 until notes.length()) Text("• " + noteText(notes.optString(k)), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
        }
    }
}

/** 復元案の変更後の表。計画が返す表には FS 名が入らないので、案の区画の FS 名を開始位置で結びつける。 */
private fun afterTable(p: ScanProposal): PartitionTable? {
    val after = p.plan?.json?.optJSONObject("after") ?: return null
    val t = runCatching { PartitionTable.parse(after.toString()) }.getOrNull() ?: return null
    val parts = p.json.optJSONArray("partitions")
    val fsByStart = HashMap<Long, String>()
    if (parts != null) for (i in 0 until parts.length()) {
        val o = parts.getJSONObject(i)
        fsByStart[o.long("startLba")] = o.optString("fs")
    }
    return t.copy(partitions = t.partitions.map { q ->
        val fs = fsByStart[q.startLba]
        if ((q.fs.isBlank() || q.fs == "unknown") && fs != null) q.copy(fs = fs) else q
    })
}

@Composable
private fun ProposalCard(p: ScanProposal, selected: Boolean, enabled: Boolean, onPick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val idx = p.json.optInt("index")
    val conf = p.json.optInt("confidence")
    val planOk = p.plan?.ok == true && p.json.optBoolean("planOk", true)
    val after = remember(p) { afterTable(p) }
    val notes = p.json.optJSONArray("notes")
    Surface(
        Modifier.fillMaxWidth().testTag("lost_prop_$idx"),
        shape = MaterialTheme.shapes.large, color = cs.surfaceContainerLow,
        border = BorderStroke(if (selected) 3.dp else 1.dp, if (selected) cs.primary else cs.outlineVariant),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("${idx + 1}. " + proposalTitle(p.json), style = MaterialTheme.typography.titleSmall)
            ChipFlow {
                Chip(stringResource(R.string.dt_lost_confidence, conf), severity = confidenceSeverity(conf))
                Chip(if (p.json.optString("table") == "gpt") "GPT" else if (p.json.optString("table") == "mbr") "MBR" else "—")
                if (!planOk) Chip(stringResource(R.string.dt_lost_not_applicable), severity = Severity.Error)
            }
            if (notes != null) for (k in 0 until notes.length()) Text("• " + noteText(notes.optString(k)), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
            p.planError?.let { DiagLine(Severity.Error, it) }
            diagnostics(p.json.optJSONArray("problems")).forEach { (sev, text) -> DiagLine(sev, text) }
            if (after != null) {
                Text(stringResource(R.string.dt_lost_after), style = MaterialTheme.typography.labelLarge)
                PartitionBar(after, "lost_prop_${idx}_bar")
                TableRows(after, "lost_prop_${idx}_rows")
            }
            OutlinedButton(onClick = onPick, enabled = enabled && planOk, modifier = Modifier.fillMaxWidth().testTag("lost_prop_pick_$idx")) {
                Text(stringResource(if (selected) R.string.dt_lost_picked else R.string.dt_lost_pick))
            }
        }
    }
}
