// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

import android.os.Process
import java.io.File

/**
 * 個人版の kfsd の監督サービス。`android:process=":kfsd"` の別プロセスで動き、APK の nativeLibraryDir の libkfsd.so (-PusbmemKfsd=true のビルドだけが持つ。GPL-2.0-or-later) を起こして見張る。
 * libkfsd.so が無いビルド (公開版など) では、起こそうとして ENOENT で失敗する (本体は ENOPKG として扱う)。docs/kfsd.md
 */
class KfsdService : KfsdServiceBase() {
    override fun exeFile() = File(applicationInfo.nativeLibraryDir, "libkfsd.so")
    override fun allowCaller(uid: Int) = uid == Process.myUid()
}
