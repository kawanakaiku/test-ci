package org.usbmem.app

import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface UiState {
    data object Idle : UiState
    data class Loaded(val label: String, val table: PartitionTable) : UiState
    data class Failed(val message: String) : UiState
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { MaterialTheme { Scaffold { pad -> Screen(Modifier.padding(pad)) } } }
    }

    @Composable
    private fun Screen(modifier: Modifier) {
        var state by remember { mutableStateOf<UiState>(UiState.Idle) }
        val scope = rememberCoroutineScope()
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri != null) scope.launch { state = withContext(Dispatchers.IO) { load(uri) } }
        }
        Column(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("ディスクイメージ(読み取り専用)", style = MaterialTheme.typography.titleLarge)
            Button(onClick = { picker.launch(arrayOf("*/*")) }) { Text("RAW イメージを開く") }
            when (val s = state) {
                UiState.Idle -> Text("イメージ(.img / .bin)を選ぶとパーティションを一覧表示します。書き込みは行いません。")
                is UiState.Failed -> Text("エラー: ${s.message}", color = MaterialTheme.colorScheme.error)
                is UiState.Loaded -> TableView(s)
            }
        }
    }

    private fun load(uri: Uri): UiState = try {
        // 読み取り専用 ("r") で開く。fd の所有権はネイティブ側へ渡す。
        val pfd = contentResolver.openFileDescriptor(uri, "r") ?: return UiState.Failed("開けません")
        val fd = pfd.detachFd()
        RawImage.openReadOnly(fd).use { img -> UiState.Loaded(uri.lastPathSegment ?: "image", img.partitionTable()) }
    } catch (e: NativeException) {
        UiState.Failed(e.message ?: "native error")
    } catch (e: Exception) {
        UiState.Failed(e.javaClass.simpleName)  // パスや内容は出さない
    }

    @Composable
    private fun TableView(s: UiState.Loaded) {
        val t = s.table
        Text("${s.label}: ${t.kind.uppercase()} / ${formatBytes(t.diskBytes)} / セクタ ${t.sectorSize}B")
        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(t.partitions) { p ->
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp)) {
                        Row { Text("#${p.index} ${p.name}".trim(), style = MaterialTheme.typography.titleMedium) }
                        Text("${formatBytes(p.sizeBytes)}  開始 LBA ${p.startLba}  ${p.typeName}  [${p.fs}]" +
                            if (p.extendedContainer) "  (拡張枠)" else "")
                    }
                }
            }
            items(t.diagnostics) { d -> Text("${d.severity}: ${d.message}") }
        }
    }
}
