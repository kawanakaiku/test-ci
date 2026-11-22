package org.usbmem.kfsd

import android.content.pm.PackageManager
import android.os.Process
import org.usbmem.app.kfsd.KfsdServiceBase
import java.io.File

/**
 * kfsd のプラグインのサービス (GPL-2.0-or-later。このプラグイン APK の libkfsd.so と一緒にソースを別に公開する)。
 * 本体から Binder で、socketpair の端を受け取り、libkfsd.so を `--fd=3` で起こして見張る ([KfsdServiceBase])。
 * 呼び出し元は、署名がこの APK と同じアプリだけ (試作の信頼の根拠。配布では固定した証明書の一覧との照合に置き換える)。
 */
class PluginService : KfsdServiceBase() {
    override fun exeFile() = File(applicationInfo.nativeLibraryDir, "libkfsd.so")

    override fun allowCaller(uid: Int): Boolean =
        uid == Process.myUid() || packageManager.checkSignatures(uid, Process.myUid()) == PackageManager.SIGNATURE_MATCH
}
