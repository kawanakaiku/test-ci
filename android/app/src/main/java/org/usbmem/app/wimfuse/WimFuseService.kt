package org.usbmem.app.wimfuse

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.Parcel
import android.os.Process

/**
 * 個人版の FUSE 層 (wimfuse) の監督サービス。`android:process=":wimfuse"` の別プロセスで動き、libwimfuse.so (wimlib の wimmount) を exec して見張る。
 * アプリ本体とは Binder の簡単な呼び出し ([Code]) だけで話し、データの通信 (RPC) は子と本体が直接 UNIX ソケットでつなぐ (このサービスは通さない)。
 * 本体がすべて unbind するとサービスが破棄され、子はすべて終了させる (onDestroy)。docs/fuse-layer.md 10 章
 */
class WimFuseService : Service() {
    object Code {
        const val SPAWN = 1      // in: String socketName, Int pdeathsig → out: Int pid
        const val ALIVE = 2      // in: Int pid → out: Int (1 / 0)
        const val EXIT = 3       // in: Int pid → out: Int (終了コード。生きていれば Int.MIN_VALUE)
        const val KILL = 4       // in: Int pid
        const val TAIL = 5       // in: Int pid → out: String
        const val SELF = 6       // out: Int このサービスのプロセスの pid
        const val VERSION = 7    // out: String (libwimfuse.so --version の出力)
    }

    private var spawner: ChildSpawner? = null

    override fun onCreate() {
        super.onCreate()
        spawner = ChildSpawner(WimFuse.exeFile(this))
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        spawner?.shutdown()
        super.onDestroy()
    }

    private val binder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            val sp = spawner ?: return false
            try {
                when (code) {
                    Code.SPAWN -> { val name = data.readString() ?: ""; val pd = data.readInt() != 0; val pid = sp.spawn(name, pd); reply?.writeInt(0); reply?.writeInt(pid) }
                    Code.ALIVE -> { reply?.writeInt(0); reply?.writeInt(if (sp.alive(data.readInt())) 1 else 0) }
                    Code.EXIT -> { reply?.writeInt(0); reply?.writeInt(sp.exitValue(data.readInt()) ?: Int.MIN_VALUE) }
                    Code.KILL -> { sp.kill(data.readInt()); reply?.writeInt(0) }
                    Code.TAIL -> { reply?.writeInt(0); reply?.writeString(sp.tail(data.readInt())) }
                    Code.SELF -> { reply?.writeInt(0); reply?.writeInt(Process.myPid()) }
                    Code.VERSION -> { reply?.writeInt(0); reply?.writeString(sp.version()) }
                    else -> return super.onTransact(code, data, reply, flags)
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
