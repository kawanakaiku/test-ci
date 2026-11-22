// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.PartitionTable
import org.usbmem.app.R
import org.usbmem.app.formatBytes

/**
 * ディスクツールの入口 (USB 画面のデバイス / パーティション、イメージ画面のイメージから [DiskToolsNav.open] で入る)。
 * ツール一覧 → 各ツールの画面。戻るボタンで一覧 → 元の画面へ。実処理は [DiskToolsActions]。docs/disk-ui.md
 */
@Composable
fun DiskToolsHost(a: DiskToolsActions, modifier: Modifier = Modifier) {
    val target = DiskToolsNav.target ?: return
    val scope = rememberUiScope()
    var table by remember(target) { mutableStateOf<PartitionTable?>(null) }
    var loadError by remember(target) { mutableStateOf<String?>(null) }
    var rangePref by remember(target) { mutableIntStateOf(-1) }
    fun reload() { scope.launch { val r = withContext(Dispatchers.IO) { a.readTable(target) }; r.value?.let { table = it }; loadError = r.error } }
    LaunchedEffectUi(target) { reload() }
    val tool = DiskToolsNav.tool
    BackHandler { if (tool != null) DiskToolsNav.tool = null else DiskToolsNav.close() }
    val writeAllowed = a.writeAllowed(target)

    Column(modifier.testTag("disk_tools_host"), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        // ---- 見出し: 戻る / どの対象か / 書き込みモード
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            IconButton(onClick = { if (tool != null) DiskToolsNav.tool = null else DiskToolsNav.close() }, modifier = Modifier.testTag("disk_back")) {
                Icon(painterResource(R.drawable.ic_dt_back), contentDescription = stringResource(R.string.dt_back))
            }
            Column(Modifier.weight(1f)) {
                Text(
                    if (tool == null) stringResource(R.string.dt_title) else stringResource(tool.title),
                    style = MaterialTheme.typography.titleLarge, modifier = Modifier.semantics { heading() }.testTag("disk_title"),
                )
                Text(target.title, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("disk_target_title"))
            }
            Chip(
                stringResource(if (writeAllowed) R.string.dt_mode_full else R.string.common_mode_read_only),
                severity = if (writeAllowed) Severity.Error else Severity.Success, modifier = Modifier.testTag("disk_mode_chip"),
            )
        }
        val t = table
        if (loadError != null) Banner(Notice(Severity.Error, stringResource(R.string.dt_load_failed), loadError), Modifier.testTag("disk_load_error"))
        if (t == null) {
            if (loadError == null) Text(stringResource(R.string.dt_loading), style = MaterialTheme.typography.bodyMedium)
            return@Column
        }
        if (tool == null) {
            Hub(target, writeAllowed, t, onOpen = { DiskToolsNav.tool = it })
        } else {
            val ctx = ToolContext(a, target, t, writeAllowed, reload = { reload() }, openTool = { to, idx -> rangePref = idx; DiskToolsNav.tool = to }, initialRange = rangePref)
            when (tool) {
                DiskTool.Check -> DiskCheckScreen(ctx)
                DiskTool.Format -> DiskFormatScreen(ctx)
                DiskTool.Partition -> DiskPartitionScreen(ctx)
                DiskTool.Backup -> DiskBackupScreen(ctx)
                DiskTool.Rescue -> DiskRescueScreen(ctx)
            }
        }
    }
}

@Composable
private fun Hub(target: DiskTarget, writeAllowed: Boolean, table: PartitionTable, onOpen: (DiskTool) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(
            stringResource(R.string.dt_hub_summary, table.kind.uppercase(), formatBytes(table.diskBytes), table.partitions.count { !it.extendedContainer && !it.protective }),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        PartitionBar(table, "hub_bar")
        if (!writeAllowed) {
            Banner(
                Notice(Severity.Info, stringResource(R.string.dt_hub_readonly_title), stringResource(if (target.kind == DiskTargetKind.Usb) R.string.dt_hub_readonly_usb else R.string.dt_hub_readonly_image)),
                Modifier.testTag("disk_hub_readonly_notice"),
            )
        }
        Text(stringResource(R.string.dt_hub_readonly_group), style = MaterialTheme.typography.titleSmall, modifier = Modifier.semantics { heading() })
        DiskTool.entries.filter { !it.destructive }.forEach { ToolCard(it, writeAllowed, onOpen) }
        Text(stringResource(R.string.dt_hub_write_group), style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.error, modifier = Modifier.semantics { heading() })
        DiskTool.entries.filter { it.destructive }.forEach { ToolCard(it, writeAllowed, onOpen) }
    }
}

@Composable
private fun ToolCard(tool: DiskTool, writeAllowed: Boolean, onOpen: (DiskTool) -> Unit) {
    val cs = MaterialTheme.colorScheme
    val st = LocalStatusColors.current
    val danger = tool.destructive
    val label = stringResource(tool.title)
    val badge = stringResource(if (danger) R.string.dt_badge_write else R.string.dt_badge_read_only)
    Surface(
        Modifier.fillMaxWidth().testTag(tool.tag).clickable(role = Role.Button, onClickLabel = label) { onOpen(tool) },
        shape = MaterialTheme.shapes.large,
        color = if (danger) cs.errorContainer.copy(alpha = 0.35f) else st.successContainer.copy(alpha = 0.55f),
        border = androidx.compose.foundation.BorderStroke(if (danger) 2.dp else 1.dp, if (danger) cs.error else cs.outlineVariant),
    ) {
        Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(painterResource(tool.icon), contentDescription = null, tint = if (danger) cs.error else st.onSuccessContainer, modifier = Modifier.size(28.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f, fill = false))
                    Chip(badge, severity = if (danger) Severity.Error else Severity.Success)
                }
                Text(stringResource(tool.subtitle), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                if (danger && !writeAllowed && tool != DiskTool.Backup) Text(stringResource(R.string.dt_hub_needs_write), style = MaterialTheme.typography.bodySmall, color = cs.error)
            }
        }
    }
}

/**
 * USB 画面 / イメージ画面に置く、ディスクツールへの入口ボタン。[DiskTarget.preselectStart] で、そのパーティションを選んだ状態で入れる。
 * [compact] = パーティションの行に置く小さいボタン。
 */
@Composable
fun DiskToolsEntryButton(target: DiskTarget, modifier: Modifier = Modifier, tag: String = "disk_tools_entry", compact: Boolean = false, enabled: Boolean = true) {
    val label = stringResource(if (compact) R.string.dt_entry_partition else R.string.dt_entry)
    if (compact) {
        androidx.compose.material3.TextButton(onClick = { DiskToolsNav.open(target) }, enabled = enabled, modifier = modifier.testTag(tag)) { Text(label) }
    } else {
        OutlinedButton(onClick = { DiskToolsNav.open(target) }, enabled = enabled, modifier = modifier.testTag(tag)) {
            Icon(painterResource(R.drawable.ic_tune), contentDescription = null, modifier = Modifier.size(18.dp))
            Text(label, modifier = Modifier.padding(start = 8.dp))
        }
    }
}
