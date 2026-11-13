package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import org.usbmem.app.R

/** 診断画面の操作 (実処理は MainActivity)。どれも [key] (UsbDevice.deviceName) のデバイスだけが対象。 */
interface DiagnosticsActions {
    fun refreshOps(key: String)
    fun benchmark(key: String)
    fun optimize(key: String)
    fun writeSelfTest(key: String)
}

/** 開いている USB ごとに、診断のカードを出す (見出しにデバイス名)。 */
@Composable
fun DiagnosticsScreen(m: AppModel, a: DiagnosticsActions, modifier: Modifier = Modifier) {
    val open = m.usbDevices.filter { it.open }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        if (open.isEmpty()) Banner(Notice(Severity.Info, stringResource(R.string.diag_no_open_device)))
        open.forEach { d -> key(d.key) { DeviceDiagnostics(d, a) } }
    }
}

@Composable
private fun DeviceDiagnostics(d: UsbDeviceState, a: DiagnosticsActions) {
    // 速度計測と最適化は、公開 (マウント) 前の読み取りだけの処理。公開中は転送サイズを変えられない (読み取り中の設定変更を避ける)
    val canTune = d.open && d.published.isEmpty() && d.busy == null
    val selfTestVolume = d.published.any { it.fullWrite }
    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(stringResource(R.string.diag_device_header, d.title), style = MaterialTheme.typography.titleMedium)
        SectionCard(stringResource(R.string.diag_speed_title), subtitle = stringResource(R.string.diag_speed_subtitle)) {
            KeyValue(stringResource(R.string.diag_current_setting), d.tuningSaved ?: stringResource(R.string.tuning_default))
            Text(
                stringResource(R.string.diag_optimize_desc),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = { a.optimize(d.key) }, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_optimize_button)) }
            OutlinedButton(onClick = { a.benchmark(d.key) }, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_bench_button)) }
            if (!canTune) Text(stringResource(if (!d.open) R.string.diag_hint_need_open else if (d.published.isNotEmpty()) R.string.diag_hint_published else R.string.diag_hint_busy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (d.tuneLog.isNotEmpty()) Text(d.tuneLog, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        }
        if (selfTestVolume) SectionCard(stringResource(R.string.diag_selftest_title), subtitle = stringResource(R.string.diag_selftest_subtitle)) {
            Text(stringResource(R.string.diag_selftest_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Button(onClick = { a.writeSelfTest(d.key) }, enabled = d.busy == null, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_selftest_button)) }
            d.selfTestPassed?.let { Text(stringResource(if (it) R.string.diag_selftest_passed else R.string.diag_selftest_failed), style = MaterialTheme.typography.titleSmall) }
            if (d.selfTestLog.isNotEmpty()) Text(d.selfTestLog, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        }
        SectionCard(stringResource(R.string.diag_media_title), subtitle = stringResource(R.string.diag_media_subtitle)) {
            KeyValue(stringResource(R.string.usb_media_title), mediaKindLabel(d.mediaKind))
            Text(mediaBasisLabel(d.mediaKind), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(d.mediaKind.detail.ifEmpty { "-" }, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
            Text(stringResource(R.string.diag_media_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        SectionCard(stringResource(R.string.diag_scsi_title), subtitle = stringResource(R.string.diag_scsi_subtitle)) {
            Text(if (d.ops.isEmpty()) stringResource(R.string.diag_scsi_empty) else d.ops, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
            Text(
                stringResource(R.string.diag_scsi_desc),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = { a.refreshOps(d.key) }, enabled = d.open, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.diag_scsi_refresh)) }
        }
    }
}
