package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import org.usbmem.app.R

/** 診断画面の操作 (実処理は MainActivity)。 */
interface DiagnosticsActions {
    fun refreshOps()
    fun benchmark()
    fun optimize()
    fun writeSelfTest()
}

@Composable
fun DiagnosticsScreen(m: AppModel, a: DiagnosticsActions, modifier: Modifier = Modifier) {
    val open = m.usbOpenKey != null
    // 速度計測と最適化は、公開 (マウント) 前の読み取りだけの処理。公開中は転送サイズを変えられない (読み取り中の設定変更を避ける)
    val canTune = open && m.published == null && m.busy == null
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        SectionCard(stringResource(R.string.diag_speed_title), subtitle = stringResource(R.string.diag_speed_subtitle)) {
            KeyValue(stringResource(R.string.diag_current_setting), m.tuningSaved ?: stringResource(R.string.tuning_default))
            Text(
                stringResource(R.string.diag_optimize_desc),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = a::optimize, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_optimize_button)) }
            OutlinedButton(onClick = a::benchmark, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_bench_button)) }
            if (!canTune) Text(stringResource(if (!open) R.string.diag_hint_need_open else if (m.published != null) R.string.diag_hint_published else R.string.diag_hint_busy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (m.tuneLog.isNotEmpty()) Text(m.tuneLog, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        }
        if (m.published?.fullWrite == true) SectionCard(stringResource(R.string.diag_selftest_title), subtitle = stringResource(R.string.diag_selftest_subtitle)) {
            Text(stringResource(R.string.diag_selftest_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Button(onClick = a::writeSelfTest, enabled = m.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_selftest_button)) }
            m.selfTestPassed?.let { Text(stringResource(if (it) R.string.diag_selftest_passed else R.string.diag_selftest_failed), style = MaterialTheme.typography.titleSmall) }
            if (m.selfTestLog.isNotEmpty()) Text(m.selfTestLog, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        }
        SectionCard(stringResource(R.string.diag_scsi_title), subtitle = stringResource(R.string.diag_scsi_subtitle)) {
            Text(if (m.ops.isEmpty()) stringResource(R.string.diag_scsi_empty) else m.ops, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
            Text(
                stringResource(R.string.diag_scsi_desc),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = a::refreshOps, enabled = open, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_scsi_refresh)) }
        }
    }
}
