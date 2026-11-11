package org.usbmem.app.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.LinkResolver
import org.usbmem.app.NtfsEntry
import org.usbmem.app.R

/** マウント中のボリューム上での、アプリ内のリンク作成。ファイルアプリ (SAF) にはリンクを作る操作が無いため、ここで行う。 */
interface LinkActions {
    /** ディレクトリの中身 (名前順ではない。呼び出し側で並べる)。IO スレッドから呼ぶ。失敗は例外。 */
    fun listDir(path: String): List<NtfsEntry>
    /** リンクを作る。成功で null、失敗でエラー文。[hard] = ハードリンク。IO スレッドから呼ぶ。 */
    fun createLink(hard: Boolean, folder: String, name: String, target: String): String?
    /** 作成に成功した後の通知 (画面の通知バナーを出す)。UI スレッドから呼ぶ。 */
    fun linkCreated(name: String)
}

@Composable
fun LinkDialog(a: LinkActions, allowHard: Boolean, onDone: (String) -> Unit, onDismiss: () -> Unit) {
    var hard by remember { mutableStateOf(false) }
    var folder by remember { mutableStateOf("/") }
    var name by remember { mutableStateOf("") }
    var target by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var working by remember { mutableStateOf(false) }
    var pick by remember { mutableStateOf<Int?>(null) }   // 1 = 作る場所を選ぶ, 2 = リンク先/元のファイルを選ぶ
    val scope = rememberCoroutineScope()

    AlertDialog(
        onDismissRequest = { if (!working) onDismiss() },
        title = { Text(stringResource(R.string.link_title)) },
        text = {
            Column(Modifier.heightIn(max = 460.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = !hard, onClick = { hard = false }, label = { Text(stringResource(R.string.link_kind_symlink)) })
                    if (allowHard) FilterChip(selected = hard, onClick = { hard = true }, label = { Text(stringResource(R.string.link_kind_hard)) })
                }
                Text(stringResource(if (hard) R.string.link_desc_hard else R.string.link_desc_symlink), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                OutlinedTextField(folder, { folder = it }, label = { Text(stringResource(R.string.link_folder)) }, singleLine = true, modifier = Modifier.fillMaxWidth(), trailingIcon = { TextButton(onClick = { pick = 1 }) { Text(stringResource(R.string.link_pick)) } })
                OutlinedTextField(name, { name = it }, label = { Text(stringResource(R.string.link_name)) }, singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(target, { target = it }, label = { Text(stringResource(if (hard) R.string.link_target_hard else R.string.link_target_symlink)) }, singleLine = true, modifier = Modifier.fillMaxWidth(), trailingIcon = { TextButton(onClick = { pick = 2 }) { Text(stringResource(R.string.link_pick)) } })
                error?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error) }
            }
        },
        confirmButton = {
            TextButton(enabled = !working && name.isNotBlank() && target.isNotBlank(), onClick = {
                working = true; error = null
                scope.launch {
                    val r = withContext(Dispatchers.IO) { try { a.createLink(hard, folder.ifBlank { "/" }, name.trim(), target.trim()) } catch (e: Exception) { e.message ?: e.javaClass.simpleName } }
                    working = false
                    if (r == null) onDone(name.trim()) else error = r
                }
            }) { Text(stringResource(R.string.link_create)) }
        },
        dismissButton = { TextButton(enabled = !working, onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) } },
    )

    pick?.let { which ->
        PathPicker(
            a, start = if (which == 1) folder else folder, foldersOnly = which == 1,
            onPick = { p ->
                if (which == 1) folder = p
                else target = if (hard) p else LinkResolver.relative(folder, p)   // シンボリックリンクは、作る場所からの相対パスにする (別の機器でマウントしても切れない)
                pick = null
            },
            onDismiss = { pick = null },
        )
    }
}

/** ボリュームの中を辿って、フォルダ (または項目) を 1 つ選ぶ。 */
@Composable
private fun PathPicker(a: LinkActions, start: String, foldersOnly: Boolean, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    var dir by remember { mutableStateOf(if (start.startsWith("/")) start else "/") }
    var items by remember { mutableStateOf<List<NtfsEntry>>(emptyList()) }
    var err by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(dir) {
        err = null
        items = try { withContext(Dispatchers.IO) { a.listDir(dir).sortedWith(compareBy({ !it.isDir }, { it.name.lowercase() })) } }
        catch (e: Exception) { err = e.message ?: e.javaClass.simpleName; emptyList() }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(dir, style = MaterialTheme.typography.titleSmall) },
        text = {
            Column {
                if (dir != "/") Text(stringResource(R.string.link_up), Modifier.fillMaxWidth().clickable { dir = dir.substringBeforeLast('/', "").ifEmpty { "/" } }.padding(vertical = 10.dp), style = MaterialTheme.typography.bodyLarge)
                err?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
                LazyColumn(Modifier.heightIn(max = 360.dp)) {
                    items(items) { e ->
                        val path = LinkResolver.joinPath(dir, e.name)
                        val label = (if (e.isLink) "↪ " else "") + e.name + if (e.isDir) "/" else ""
                        Text(label, Modifier.fillMaxWidth().clickable { if (e.isDir) dir = path else if (!foldersOnly) onPick(path) }.padding(vertical = 10.dp), style = MaterialTheme.typography.bodyLarge)
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = { onPick(dir) }) { Text(stringResource(R.string.link_pick_here)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) } },
    )
}
