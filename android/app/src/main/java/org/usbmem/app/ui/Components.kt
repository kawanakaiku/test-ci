package org.usbmem.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.usbmem.app.R

/** 画面に出す通知の重さ。色とアイコンが決まる。 */
enum class Severity { Info, Success, Warning, Error }

data class Notice(val severity: Severity, val text: String, val detail: String? = null)

@Composable
private fun severityColors(s: Severity): Pair<Color, Color> {
    val st = LocalStatusColors.current
    val cs = MaterialTheme.colorScheme
    return when (s) {
        Severity.Info -> cs.secondaryContainer to cs.onSecondaryContainer
        Severity.Success -> st.successContainer to st.onSuccessContainer
        Severity.Warning -> st.warningContainer to st.onWarningContainer
        Severity.Error -> cs.errorContainer to cs.onErrorContainer
    }
}

private fun severityIcon(s: Severity) = when (s) {
    Severity.Info -> R.drawable.ic_info
    Severity.Success -> R.drawable.ic_check
    Severity.Warning -> R.drawable.ic_warning
    Severity.Error -> R.drawable.ic_error
}

/** 状態・結果の通知。成功/情報/警告/エラーで色とアイコンを変える (以前は成功もエラーと同じ赤字だった)。 */
@Composable
fun Banner(notice: Notice, modifier: Modifier = Modifier, action: (@Composable () -> Unit)? = null) {
    val (bg, fg) = severityColors(notice.severity)
    Surface(modifier.fillMaxWidth(), color = bg, contentColor = fg, shape = MaterialTheme.shapes.medium) {
        Row(Modifier.padding(14.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(painterResource(severityIcon(notice.severity)), contentDescription = null, Modifier.size(22.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(notice.text, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
                if (notice.detail != null) Text(notice.detail, style = MaterialTheme.typography.bodySmall)
                if (action != null) Box(Modifier.padding(top = 4.dp)) { action() }
            }
        }
    }
}

/** 見出し付きのまとまり。画面の機能を足すときは、これを 1 枚足す。 */
@Composable
fun SectionCard(title: String, modifier: Modifier = Modifier, subtitle: String? = null, trailing: (@Composable () -> Unit)? = null, content: @Composable () -> Unit) {
    Card(modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow), shape = MaterialTheme.shapes.large) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleMedium)
                    if (subtitle != null) Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (trailing != null) trailing()
            }
            content()
        }
    }
}

/** 小さな状態表示 (モード・ファイルシステム名など)。 */
@Composable
fun Chip(text: String, modifier: Modifier = Modifier, severity: Severity = Severity.Info) {
    val (bg, fg) = severityColors(severity)
    Surface(modifier, color = bg, contentColor = fg, shape = CircleShape) {
        Text(text, Modifier.padding(horizontal = 10.dp, vertical = 4.dp), style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
fun KeyValue(key: String, value: String, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
        Text(key, Modifier.width(112.dp), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
    }
}

/** 手順の 1 行 (番号つき)。done = 済んだ手順。 */
@Composable
fun Step(number: Int, title: String, body: String? = null, done: Boolean = false) {
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
        Surface(Modifier.size(24.dp), shape = CircleShape, color = if (done) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainerHighest) {
            Box(contentAlignment = Alignment.Center) {
                Text("$number", style = MaterialTheme.typography.labelMedium, color = if (done) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
            if (body != null) Text(body, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** 取り返しのつかない/注意が要る操作の確認。誤タップ対策 (書き込み可で開いている USB を閉じるときなど)。 */
@Composable
fun ConfirmDialog(title: String, text: String, confirmLabel: String, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = { Text(text) },
        confirmButton = { TextButton(onClick = onConfirm) { Text(confirmLabel) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) } },
    )
}

@Composable
fun Gap(h: Int = 8) { Spacer(Modifier.padding(top = h.dp)) }

fun Modifier.clipShape(shape: androidx.compose.ui.graphics.Shape) = this.clip(shape)
