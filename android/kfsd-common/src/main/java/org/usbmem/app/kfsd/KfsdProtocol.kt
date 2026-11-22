// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.os.Process
import java.io.File

/** kfsd を起こす・見張るサービスとの Binder の呼び出し ([KfsdServiceBase])。データの通信 (RPC) は、本体と kfsd が socketpair で直接つなぐ (サービスは通さない)。 */
object KfsdCode {
    const val SPAWN = 1      // in: FileDescriptor (socketpair の端。子の fd 3 になる) → out: Int pid
    const val ALIVE = 2      // in: Int pid → out: Int (1 / 0)
    const val EXIT = 3       // in: Int pid → out: Int (終了コード。生きていれば Int.MIN_VALUE)
    const val KILL = 4       // in: Int pid
    const val TAIL = 5       // in: Int pid → out: String
    const val SELF = 6       // out: Int このサービスのプロセスの pid
    const val VERSION = 7    // out: String (kfsd --version の出力)
}

/**
 * kfsd を起こして見張るサービスの共通部。個人版は :kfsd プロセスの [org.usbmem.app.kfsd.KfsdService] (libkfsd.so)、配布用は別 APK のプラグインの PluginService (同じ実行ファイル)。
 * 本体がすべて unbind するとサービスが破棄され、子はすべて終了させる (onDestroy)。
 */
abstract class KfsdServiceBase : Service() {
    private var spawner: KfsdSpawner? = null

    /** 起こす kfsd の実行ファイル (自分の nativeLibraryDir の libkfsd.so)。 */
    protected abstract fun exeFile(): File

    /** 呼び出し元の uid を許すか (個人版: 自分の uid だけ。プラグイン: 署名が同じアプリだけ)。 */
    protected abstract fun allowCaller(uid: Int): Boolean

    override fun onCreate() {
        super.onCreate()
        spawner = KfsdSpawner(exeFile())
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        spawner?.shutdown()
        super.onDestroy()
    }

    private val binder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code < KfsdCode.SPAWN || code > KfsdCode.VERSION) return super.onTransact(code, data, reply, flags)
            val sp = spawner ?: return false
            if (!allowCaller(Binder.getCallingUid())) { reply?.writeInt(1); reply?.writeString("caller not allowed"); return true }
            try {
                when (code) {
                    KfsdCode.SPAWN -> {
                        val pfd = data.readFileDescriptor()
                        try { val pid = sp.spawn(pfd); reply?.writeInt(0); reply?.writeInt(pid) } finally { pfd.close() }
                    }
                    KfsdCode.ALIVE -> { reply?.writeInt(0); reply?.writeInt(if (sp.alive(data.readInt())) 1 else 0) }
                    KfsdCode.EXIT -> { reply?.writeInt(0); reply?.writeInt(sp.exitValue(data.readInt()) ?: Int.MIN_VALUE) }
                    KfsdCode.KILL -> { sp.kill(data.readInt()); reply?.writeInt(0) }
                    KfsdCode.TAIL -> { reply?.writeInt(0); reply?.writeString(sp.tail(data.readInt())) }
                    KfsdCode.SELF -> { reply?.writeInt(0); reply?.writeInt(Process.myPid()) }
                    KfsdCode.VERSION -> { reply?.writeInt(0); reply?.writeString(sp.version()) }
                }
            } catch (e: Exception) {
                reply?.setDataPosition(0)
                reply?.writeInt(1)
                reply?.writeString(e.toString())
            }
            return true
        }
    }
}
