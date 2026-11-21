// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.os.IBinder
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * kfsd (カーネル由来 = GPL 系のファイルシステム・ドライバを、別プロセスの実行ファイルに分けたもの。docs/kfsd.md) への入口。
 *
 * 本体のネイティブ層 (jni_ntfs.cpp の mount_auto) は、[apply] で「kfsd 経由」にしてあると、マウントのたびに [spawnChildFd] を呼んで kfsd を 1 つ起こし、
 * その socketpair 越しの RPC (core/kfsd の RpcVolume) でボリュームを使う。kfsd の入手先は 2 つ:
 *  - [Source.LOCAL] 個人版: APK の libkfsd.so (-PusbmemKfsd=true のビルド) を、:kfsd プロセスの [KfsdService] が起こす
 *  - [Source.PLUGIN] 別 APK のプラグイン (パッケージ [PLUGIN_PACKAGE]、GPL-2.0。ソースは別に公開) の PluginService が起こす。公開版の本体はこちらだけ
 * どちらにも無ければ ENOPKG (65) = 「この FS には kfsd (プラグイン) が必要です」。
 */
object Kfsd {
    enum class Source { AUTO, LOCAL, PLUGIN }

    const val PLUGIN_PACKAGE = "org.usbmem.kfsd"
    const val PLUGIN_SERVICE = "org.usbmem.kfsd.PluginService"
    const val ENOPKG = 65

    private const val PREFS = "usbmem_kfsd"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_SOURCE = "source"
    /** デバッグビルドだけが見る上書き (計装テスト用): 内容 = off / local / plugin / auto。 */
    private const val DEBUG_FILE = "/data/local/tmp/usbmem_kfsd"

    @Volatile private var appCtx: Context? = null
    @Volatile var source: Source = Source.AUTO
        private set

    /** 設定を読んでネイティブ層に反映する (Application.onCreate から)。公開版の既定は「kfsd 経由」、個人版の既定は「プロセス内」。 */
    fun configure(ctx: Context) {
        val app = ctx.applicationContext
        // :kfsd / :wimfuse などのサービスのプロセスでは何もしない (ネイティブ層 = usbmem_jni を読み込まない)
        val proc = try { File("/proc/self/cmdline").readText().substringBefore('\u0000') } catch (_: Exception) { app.packageName }
        if (proc != app.packageName) return
        appCtx = app
        val p = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        var enabled = p.getBoolean(KEY_ENABLED, org.usbmem.app.Edition.isPublic)
        var src = runCatching { Source.valueOf(p.getString(KEY_SOURCE, "AUTO")!!) }.getOrDefault(Source.AUTO)
        if (app.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            val o = runCatching { File(DEBUG_FILE).readText().trim().lowercase() }.getOrNull()
            when (o) {
                "off" -> enabled = false
                "local" -> { enabled = true; src = Source.LOCAL }
                "plugin" -> { enabled = true; src = Source.PLUGIN }
                "auto", "on" -> { enabled = true; src = Source.AUTO }
            }
        }
        apply(app, enabled, src)
    }

    /** 方式を切り替える (設定にも保存はしない。保存は [save])。以降のマウントから有効。マウント済みのボリュームはそのまま。 */
    fun apply(ctx: Context, enabled: Boolean, src: Source = Source.AUTO) {
        appCtx = ctx.applicationContext
        source = src
        KfsdNative.nativeSetMode(if (enabled) 1 else 0)
    }

    fun save(ctx: Context, enabled: Boolean, src: Source = Source.AUTO) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_ENABLED, enabled).putString(KEY_SOURCE, src.name).apply()
        apply(ctx, enabled, src)
    }

    /** kfsd 経由でマウントする設定か。 */
    fun enabled(): Boolean = KfsdNative.nativeGetMode() != 0

    // ---- 入手先 ----
    private fun localExe(ctx: Context) = File(ctx.applicationInfo.nativeLibraryDir, "libkfsd.so")

    /** このビルドが libkfsd.so を持っていて、実行できるか (個人版の -PusbmemKfsd=true のビルド)。 */
    fun localAvailable(ctx: Context): Boolean = localExe(ctx).let { it.isFile && it.canExecute() }

    /** 署名が同じプラグイン APK が入っているか。試作の信頼の根拠は「署名が本体と同じ」。配布では、固定した証明書の一覧 (SHA-256) との照合に置き換える (docs/kfsd.md 6 章)。 */
    fun pluginAvailable(ctx: Context): Boolean = try {
        ctx.packageManager.getPackageInfo(PLUGIN_PACKAGE, 0)
        ctx.packageManager.checkSignatures(ctx.packageName, PLUGIN_PACKAGE) == PackageManager.SIGNATURE_MATCH
    } catch (_: Exception) { false }

    /** 起こせる kfsd があるか (設定の入手先に従う)。 */
    fun available(ctx: Context): Boolean = when (source) {
        Source.LOCAL -> localAvailable(ctx)
        Source.PLUGIN -> pluginAvailable(ctx)
        Source.AUTO -> localAvailable(ctx) || pluginAvailable(ctx)
    }

    // ---- サービスへの呼び出し ----
    interface Control {
        fun spawn(sock: ParcelFileDescriptor): Int
        fun alive(pid: Int): Boolean
        fun exitValue(pid: Int): Int?
        fun kill(pid: Int)
        fun tail(pid: Int): String
        /** 監督している側のプロセスの pid (:kfsd やプラグインのサービスの pid)。 */
        fun hostPid(): Int
        fun version(): String
    }

    private class RemoteControl(val b: IBinder) : Control {
        private inline fun <T> tx(code: Int, w: (Parcel) -> Unit, r: (Parcel) -> T): T {
            val d = Parcel.obtain(); val p = Parcel.obtain()
            try {
                w(d)
                b.transact(code, d, p, 0)
                if (p.readInt() != 0) throw java.io.IOException(p.readString() ?: "kfsd service error")
                return r(p)
            } finally { d.recycle(); p.recycle() }
        }
        override fun spawn(sock: ParcelFileDescriptor) = tx(KfsdCode.SPAWN, { it.writeFileDescriptor(sock.fileDescriptor) }, { it.readInt() })
        override fun alive(pid: Int) = tx(KfsdCode.ALIVE, { it.writeInt(pid) }, { it.readInt() != 0 })
        override fun exitValue(pid: Int) = tx(KfsdCode.EXIT, { it.writeInt(pid) }, { val v = it.readInt(); if (v == Int.MIN_VALUE) null else v })
        override fun kill(pid: Int) = tx(KfsdCode.KILL, { it.writeInt(pid) }, { })
        override fun tail(pid: Int) = tx(KfsdCode.TAIL, { it.writeInt(pid) }, { it.readString() ?: "" })
        override fun hostPid() = tx(KfsdCode.SELF, { }, { it.readInt() })
        override fun version() = tx(KfsdCode.VERSION, { }, { it.readString() ?: "" })
    }

    /** kfsd が使えない (実行ファイル・プラグインが無い、サービスにつながらない)。ネイティブ層へは ENOPKG として返る。 */
    class Unavailable(message: String) : Exception(message)

    // サービスへの接続 (入手先ごとに 1 本。アプリが生きている間は保つ。サービスのプロセスが死んでいたらつなぎ直す)。
    private class Conn(val control: RemoteControl, val connection: ServiceConnection, val binder: IBinder)
    private val conns = HashMap<Source, Conn>()

    @Synchronized
    private fun connect(ctx: Context, src: Source): Conn {
        conns[src]?.let { if (it.binder.isBinderAlive) return it else { try { ctx.unbindService(it.connection) } catch (_: Exception) {}; conns.remove(src) } }
        val intent = Intent().setComponent(
            if (src == Source.LOCAL) ComponentName(ctx, KfsdService::class.java) else ComponentName(PLUGIN_PACKAGE, PLUGIN_SERVICE))
        val latch = CountDownLatch(1)
        var bound: IBinder? = null
        val c = object : ServiceConnection {
            override fun onServiceConnected(n: ComponentName, s: IBinder) { bound = s; latch.countDown() }
            override fun onServiceDisconnected(n: ComponentName) {}
            override fun onBindingDied(n: ComponentName) { latch.countDown() }
            override fun onNullBinding(n: ComponentName) { latch.countDown() }
        }
        if (!ctx.bindService(intent, c, Context.BIND_AUTO_CREATE)) throw Unavailable("bindService($src) が false")
        if (!latch.await(20, TimeUnit.SECONDS) || bound == null) { try { ctx.unbindService(c) } catch (_: Exception) {}; throw Unavailable("kfsd のサービスに接続できない ($src)") }
        return Conn(RemoteControl(bound!!), c, bound!!).also { conns[src] = it }
    }

    private fun resolve(ctx: Context): Source = when (source) {
        Source.LOCAL -> if (localAvailable(ctx)) Source.LOCAL else throw Unavailable("libkfsd.so が無い (-PusbmemKfsd=true のビルドではない)")
        Source.PLUGIN -> if (pluginAvailable(ctx)) Source.PLUGIN else throw Unavailable("kfsd のプラグイン ($PLUGIN_PACKAGE) が入っていない、または署名が違う")
        Source.AUTO -> if (localAvailable(ctx)) Source.LOCAL else if (pluginAvailable(ctx)) Source.PLUGIN else throw Unavailable("kfsd (libkfsd.so もプラグインも) が無い")
    }

    /** 起こした kfsd (試験・診断用)。 */
    class Child(val control: Control, val pid: Int, val src: Source)
    private val spawned = java.util.concurrent.CopyOnWriteArrayList<Child>()
    fun children(): List<Child> = spawned.toList()

    /** いまの入手先のサービス (試験・診断用)。無ければ [Unavailable]。 */
    fun control(ctx: Context): Control = connect(ctx.applicationContext, resolve(ctx.applicationContext)).control

    /** kfsd を 1 つ起こして、本体側の接続済みソケットの fd (detach 済み。所有権は呼び出し側へ) を返す。失敗は -errno。ネイティブ層 (jni_kfsd.cpp) から [KfsdNative.spawnChildFd] 経由で呼ばれる。 */
    @JvmStatic fun spawnChildFd(): Int {
        val ctx = appCtx ?: return -ENOPKG
        return try {
            val src = resolve(ctx)
            val conn = connect(ctx, src)
            val pair = ParcelFileDescriptor.createSocketPair()   // [0] = 本体 (ネイティブへ渡す)、[1] = kfsd (サービスへ渡し、子の fd 3 になる)
            try {
                val pid = conn.control.spawn(pair[1])
                spawned += Child(conn.control, pid, src)
                pair[0].detachFd()
            } catch (e: Exception) {
                try { pair[0].close() } catch (_: Exception) {}
                throw e
            } finally { try { pair[1].close() } catch (_: Exception) {} }
        } catch (e: Unavailable) {
            Log.i("kfsd", "unavailable: ${e.message}")
            -ENOPKG
        } catch (e: java.io.IOException) {
            Log.w("kfsd", "spawn failed: $e")
            if (e.message?.contains("errno 2") == true || e.message?.contains("errno 13") == true) -ENOPKG else -5
        } catch (e: Exception) {
            Log.w("kfsd", "spawn failed: $e")
            -5
        }
    }
}
