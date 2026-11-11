package org.usbmem.app.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp
import org.usbmem.app.R

/** 画面 (タブ)。機能を足すときは、ここに 1 つ足して、対応する *Screen を呼ぶ。 */
private enum class Tab(val label: String, val icon: Int) {
    Usb("USB", R.drawable.ic_storage),
    Image("イメージ", R.drawable.ic_folder),
    Diagnostics("診断", R.drawable.ic_tune),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppScaffold(model: AppModel, usb: UsbActions, image: ImageActions, diag: DiagnosticsActions) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    Scaffold(
        topBar = { CenterAlignedTopAppBar(title = { Text("USB Storage Manager") }) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEachIndexed { i, t ->
                    NavigationBarItem(selected = tab == i, onClick = { tab = i }, icon = { Icon(painterResource(t.icon), contentDescription = null) }, label = { Text(t.label) })
                }
            }
        },
    ) { pad ->
        // 画面ごとに独立してスクロールする。タブを替えても USB の状態 (model) は保たれる
        val m = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
        androidx.compose.runtime.key(tab) {   // タブごとに別のスクロール位置
            Column(Modifier.fillMaxSize().padding(pad).verticalScroll(rememberScrollState())) {
                when (Tab.entries[tab]) {
                    Tab.Usb -> UsbScreen(model, usb, m)
                    Tab.Image -> ImageScreen(image, m)
                    Tab.Diagnostics -> DiagnosticsScreen(model, diag, m)
                }
            }
        }
    }
}
