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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

/** 診断画面の操作 (実処理は MainActivity)。 */
interface DiagnosticsActions {
    fun refreshOps()
    fun benchmark()
    fun optimize()
}

@Composable
fun DiagnosticsScreen(m: AppModel, a: DiagnosticsActions, modifier: Modifier = Modifier) {
    val open = m.usbOpenKey != null
    // 速度計測と最適化は、公開 (マウント) 前の読み取りだけの処理。公開中は転送サイズを変えられない (読み取り中の設定変更を避ける)
    val canTune = open && m.published == null && m.busy == null
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        SectionCard("読み取り速度", subtitle = "端末ごとに通る転送サイズが違います") {
            KeyValue("使用中の設定", m.tuningSaved)
            Text(
                "「最適化」は、小さい設定から順に、同じ区間を読んでデータが一致するかを確かめ、通った中で最速を保存します。保存した設定は、次回から USB を開くときに使われます (書き込みの転送サイズは変えません)。失敗すると USB が切れることがあります。",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = a::optimize, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text("読み取りを最適化して保存 (約 1 分)") }
            OutlinedButton(onClick = a::benchmark, enabled = canTune, modifier = Modifier.fillMaxWidth()) { Text("速度だけ計測 (約 1 分)") }
            if (!canTune) Text(if (!open) "USB を開くと実行できます。" else if (m.published != null) "公開中は実行できません。一度閉じてください。" else "処理が終わるまでお待ちください。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (m.tuneLog.isNotEmpty()) Text(m.tuneLog, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
        }
        SectionCard("送信した SCSI 命令", subtitle = "読み取り専用で、書き込み系が出ていないことの確認用") {
            Text(if (m.ops.isEmpty()) "(USB を開くと表示されます)" else m.ops, style = MaterialTheme.typography.bodyMedium, fontFamily = FontFamily.Monospace)
            Text(
                "16 進のオペコード x 回数。読み取り系: 12 INQUIRY / 00 TUR / 03 SENSE / 1a MODE SENSE / 25, 9e CAPACITY / 28, 88 READ。書き込み系は 2a, 8a, 35 (キャッシュ同期)。これらが無ければ、ディスクへの書き込みはありません。",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = a::refreshOps, enabled = open, modifier = Modifier.fillMaxWidth()) { Text("集計を更新") }
        }
    }
}
