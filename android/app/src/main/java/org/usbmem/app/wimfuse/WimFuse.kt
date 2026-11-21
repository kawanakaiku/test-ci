package org.usbmem.app.wimfuse

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.IBinder
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import org.json.JSONObject
import org.usbmem.app.BuildConfig
import org.usbmem.app.NtfsVolume
import java.io.File
import java.security.SecureRandom
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** [WimFuse] の失敗。errno (正) と、子 (libwimfuse.so) のエラー文があれば [childMessage]。 */
class WimFuseException(val errno: Int, message: String, val childMessage: String = "") : Exception(message)

/** WIM のイメージ 1 つ分。 */
data class WimImageInfo(val index: Int, val name: String, val description: String, val dirs: Long, val files: Long, val bytes: Long, val product: String, val build: String)

/** WIM のヘッダとイメージ一覧。 */
data class WimInfo(val imageCount: Int, val bootIndex: Int, val compression: String, val chunkSize: Long, val totalBytes: Long, val pipable: Boolean, val split: Boolean, val images: List<WimImageInfo>) {
    companion object {
        fun parse(json: String): WimInfo {
            val o = JSONObject(json)
            val a = o.getJSONArray("images")
            return WimInfo(
                o.getInt("image_count"), o.getInt("boot_index"), o.getString("compression"), o.getLong("chunk_size"), o.getLong("total_bytes"),
                o.getInt("pipable") != 0, o.getInt("split") != 0,
                (0 until a.length()).map {
                    val i = a.getJSONObject(it)
                    WimImageInfo(i.getInt("index"), i.getString("name"), i.getString("description"), i.getLong("dirs"), i.getLong("files"), i.getLong("bytes"), i.getString("product"), i.getString("build"))
                })
        }
    }
}

/**
 * WIM ファイル全体を開いたもの。[volume] のルート直下に、イメージごとのフォルダ ("1 - 名前" ...) が並び、各フォルダの中身がそのイメージ ([NtfsVolume] と同じ API。読み取り専用)。
 * 子プロセスはフォルダを最初に開いたときに起こす (一覧の表示だけでは起こさない)。閉じると、起こした子もすべて終わる。
 */
class WimArchive internal constructor(val volume: NtfsVolume, val info: WimInfo, val control: WimFuse.Control, private val finish: () -> Unit) : AutoCloseable {
    private var closed = false

    @Synchronized
    override fun close() {
        if (closed) return
        closed = true
        try { volume.close() } finally { finish() }
    }
}

/** マウントした WIM のイメージ。[volume] は [NtfsVolume] と同じ API (list / stat / read。読み取り専用)。閉じると子プロセスも終わる。 */
class WimImage internal constructor(val volume: NtfsVolume, val childPid: Int, val timings: Map<String, Long>, val control: WimFuse.Control, private val release: () -> Unit) : AutoCloseable {
    private var closed = false

    /** 子の出力 (標準出力・標準エラー) の最後の数十行 (診断用)。 */
    fun childLog(): String = control.tail(childPid)

    /** 子プロセスが生きているか。 */
    fun childAlive(): Boolean = control.alive(childPid)

    @Synchronized
    override fun close() {
        if (closed) return
        closed = true
        try { volume.close() } finally { release() }   // ソケットが閉じ、子は EOF で終わる
    }
}

/**
 * 個人版の FUSE 層 (wimlib の wimmount を別プロセスで動かす)。docs/fuse-layer.md 10 章。
 *
 * 子 = APK の nativeLibraryDir の libwimfuse.so (PIE の実行ファイル。AGP の jniLibs 経由で、extractNativeLibs=true で展開される) を ProcessBuilder で exec したもの。
 * 起動は [Mode.SERVICE] (本線: `:wimfuse` プロセスのサービスが exec して見張る) か [Mode.DIRECT] (呼び出し側のプロセスで exec。試験・比較用)。
 * 本体と子の通信: 本体が抽象名前空間の UNIX ソケットを待ち受け、子が connect する (名前は乱数。接続後に相手の uid を確認)。その上で
 * ../rpc_volume.h の RPC (本体 → 子: 列挙 / getattr / read / readlink、子 → 本体: WIM ファイルの中身を読む) を話す。
 */
object WimFuse {
    enum class Mode { SERVICE, DIRECT }

    /** 子の起動・監督の呼び出し (サービス越し、または同じプロセス)。 */
    interface Control {
        fun spawn(socketName: String, pdeathsig: Boolean, cacheMb: Int): Int
        fun alive(pid: Int): Boolean
        fun exitValue(pid: Int): Int?
        fun kill(pid: Int)
        fun tail(pid: Int): String
        /** 監督している側のプロセスの pid (SERVICE = :wimfuse の pid、DIRECT = 自分)。 */
        fun hostPid(): Int
        fun version(): String
    }

    fun exeFile(ctx: Context): File = File(ctx.applicationInfo.nativeLibraryDir, "libwimfuse.so")

    /** このビルドに FUSE 層が入っていて、実行ファイルが展開・実行可能か。 */
    fun available(ctx: Context): Boolean = BuildConfig.WIMFUSE_SUPPORTED && exeFile(ctx).let { it.isFile && it.canExecute() }

    // ---- Control の実装 ----
    private class LocalControl(ctx: Context) : Control {
        val sp = ChildSpawner(exeFile(ctx))
        override fun spawn(socketName: String, pdeathsig: Boolean, cacheMb: Int) = sp.spawn(socketName, pdeathsig, cacheMb)
        override fun alive(pid: Int) = sp.alive(pid)
        override fun exitValue(pid: Int) = sp.exitValue(pid)
        override fun kill(pid: Int) = sp.kill(pid)
        override fun tail(pid: Int) = sp.tail(pid)
        override fun hostPid() = android.os.Process.myPid()
        override fun version() = sp.version()
    }

    private class RemoteControl(val b: IBinder) : Control {
        private inline fun <T> tx(code: Int, w: (Parcel) -> Unit, r: (Parcel) -> T): T {
            val d = Parcel.obtain(); val p = Parcel.obtain()
            try {
                w(d)
                b.transact(code, d, p, 0)
                if (p.readInt() != 0) throw java.io.IOException(p.readString() ?: "wimfuse service error")
                return r(p)
            } finally { d.recycle(); p.recycle() }
        }
        override fun spawn(socketName: String, pdeathsig: Boolean, cacheMb: Int) = tx(WimFuseService.Code.SPAWN, { it.writeString(socketName); it.writeInt(if (pdeathsig) 1 else 0); it.writeInt(cacheMb) }, { it.readInt() })
        override fun alive(pid: Int) = tx(WimFuseService.Code.ALIVE, { it.writeInt(pid) }, { it.readInt() != 0 })
        override fun exitValue(pid: Int) = tx(WimFuseService.Code.EXIT, { it.writeInt(pid) }, { val v = it.readInt(); if (v == Int.MIN_VALUE) null else v })
        override fun kill(pid: Int) = tx(WimFuseService.Code.KILL, { it.writeInt(pid) }, { })
        override fun tail(pid: Int) = tx(WimFuseService.Code.TAIL, { it.writeInt(pid) }, { it.readString() ?: "" })
        override fun hostPid() = tx(WimFuseService.Code.SELF, { }, { it.readInt() })
        override fun version() = tx(WimFuseService.Code.VERSION, { }, { it.readString() ?: "" })
    }

    // ---- サービスへの接続 (参照数つき。最後の release で unbind → サービスが破棄され、子は全部終わる) ----
    private var local: LocalControl? = null
    private var remote: RemoteControl? = null
    private var conn: ServiceConnection? = null
    private var refs = 0

    @Synchronized
    private fun acquire(ctx: Context, mode: Mode): Control {
        val app = ctx.applicationContext
        if (mode == Mode.DIRECT) {
            refs++
            return local ?: LocalControl(app).also { local = it }
        }
        // サービスのプロセスが死んでいたら (試験の強制終了・クラッシュ)、古い接続を捨てて、つなぎ直す。保持者の数 (refs) はそのまま (全員が release するまで接続を保つ)
        if (remote != null && !remote!!.b.isBinderAlive) {
            conn?.let { try { app.unbindService(it) } catch (_: Exception) {} }
            conn = null; remote = null
        }
        if (remote == null) {
            val latch = CountDownLatch(1)
            var bound: IBinder? = null
            val c = object : ServiceConnection {
                override fun onServiceConnected(n: ComponentName, s: IBinder) { bound = s; latch.countDown() }
                override fun onServiceDisconnected(n: ComponentName) {}
                override fun onBindingDied(n: ComponentName) { latch.countDown() }
                override fun onNullBinding(n: ComponentName) { latch.countDown() }
            }
            if (!app.bindService(Intent(app, WimFuseService::class.java), c, Context.BIND_AUTO_CREATE)) throw WimFuseException(2, "bindService(WimFuseService) が false")
            if (!latch.await(20, TimeUnit.SECONDS) || bound == null) { app.unbindService(c); throw WimFuseException(110, "WimFuseService に接続できない (タイムアウト)") }
            remote = RemoteControl(bound!!)
            conn = c
        }
        refs++
        return remote!!
    }

    @Synchronized
    private fun release(ctx: Context, mode: Mode) {
        refs--
        if (refs <= 0) {
            refs = 0
            conn?.let { try { ctx.applicationContext.unbindService(it) } catch (_: Exception) {} }
            conn = null; remote = null
        }
    }

    /** 試験・診断用: サービスへの接続を保持する (子が終わったあとにサービス側の子の出力を読めるように、サービスの破棄を止める)。閉じると解放。 */
    fun hold(ctx: Context, mode: Mode = Mode.SERVICE): Control {
        val c = acquire(ctx, mode)
        return object : Control by c, AutoCloseable { override fun close() { release(ctx, mode) } }
    }

    /** exec が成立するか (`libwimfuse.so --version` の出力。例: `wimfuse wimlib 1.14.4 pid 1234 (exit 0)`)。失敗は例外。 */
    fun version(ctx: Context, mode: Mode = Mode.SERVICE): String {
        val c = acquire(ctx, mode)
        try { return c.version() } finally { release(ctx, mode) }
    }

    /** 子を起こして、本体側の接続済みソケットの fd (detach 済み) と子の pid を返す。 */
    private fun connect(ctx: Context, control: Control, pdeathsig: Boolean, cacheMb: Int, timings: MutableMap<String, Long>): Pair<Int, Int> {
        val t0 = SystemClock.elapsedRealtime()
        val name = "usbmem-wimfuse-" + ByteArray(12).also { SecureRandom().nextBytes(it) }.joinToString("") { "%02x".format(it) }
        val server = LocalServerSocket(name)
        var pid = -1
        try {
            val accepted = CompletableFuture<LocalSocket>()
            Thread({ try { accepted.complete(server.accept()) } catch (e: Exception) { accepted.completeExceptionally(e) } }, "wimfuse-accept").apply { isDaemon = true; start() }
            pid = try { control.spawn(name, pdeathsig, cacheMb) } catch (e: java.io.IOException) { throw WimFuseException(13, "libwimfuse.so を exec できない: ${e.message}") }
            timings["spawn_ms"] = SystemClock.elapsedRealtime() - t0
            val sock = try { accepted.get(20, TimeUnit.SECONDS) } catch (e: Exception) {
                val tail = try { control.tail(pid) } catch (_: Exception) { "" }
                control.kill(pid)
                throw WimFuseException(110, "子プロセスが接続してこない: ${e.javaClass.simpleName}", tail)
            }
            try {
                if (sock.peerCredentials.uid != android.os.Process.myUid()) { control.kill(pid); throw WimFuseException(1, "接続してきた相手の uid が違う") }
                val pfd = ParcelFileDescriptor.dup(sock.fileDescriptor)
                timings["connect_ms"] = SystemClock.elapsedRealtime() - t0
                return pfd.detachFd() to pid
            } finally { sock.close() }
        } finally { try { server.close() } catch (_: Exception) {} }
    }

    // ネイティブの WimVolume が、フォルダを最初に開いたときに呼ぶ起こし方 (token で引く)
    private val spawners = java.util.concurrent.ConcurrentHashMap<Long, () -> Int>()
    private val nextToken = java.util.concurrent.atomic.AtomicLong(1)
    internal fun spawnFor(token: Long): Int = spawners[token]?.invoke() ?: -9

    /**
     * WIM 全体 (dev = WIM を読む device ハンドル) を開く。ルート直下がイメージごとのフォルダのボリューム ([WimArchive.volume])。
     * 開くときに子を 1 つ起こしてイメージ一覧を読み (すぐ終わらせる)、各イメージの子はフォルダを最初に開いたときに起こす。
     * [cacheMb] = 子の中の WIM の読み取りキャッシュ (MiB。0 = なし)。dev は [WimArchive] を閉じるまで開いておくこと。
     */
    fun openArchive(ctx: Context, dev: Long, mode: Mode = Mode.SERVICE, cacheMb: Int = DEFAULT_CACHE_MB): WimArchive {
        val control = acquire(ctx, mode)
        val token = nextToken.getAndIncrement()
        var ok = false
        try {
            spawners[token] = {
                try { connect(ctx, control, true, cacheMb, HashMap()).first }
                catch (e: WimFuseException) { -e.errno }
                catch (e: Exception) { -5 }
            }
            val info = arrayOfNulls<ByteArray>(1)
            val err = IntArray(1)
            val h = WimFuseNative.nativeOpenArchive(dev, token, info, err)
            if (h == 0L) fail("wim open", if (err[0] != 0) err[0] else 5)
            val v = NtfsVolume.adoptHandle(h)
            val parsed = try { WimInfo.parse(String(info[0]!!, Charsets.UTF_8)) } catch (e: Exception) { v.close(); throw WimFuseException(5, "イメージ一覧を読めない: $e") }
            ok = true
            return WimArchive(v, parsed, control) { spawners.remove(token); release(ctx, mode) }
        } finally { if (!ok) { spawners.remove(token); release(ctx, mode) } }
    }

    /** 子の WIM 読み取りキャッシュの既定 (MiB)。 */
    const val DEFAULT_CACHE_MB = 32

    private fun fail(op: String, errno: Int): Nothing {
        val msg = String(WimFuseNative.nativeLastMessage(), Charsets.UTF_8)
        throw WimFuseException(errno, "$op failed: errno $errno ${if (msg.isNotEmpty()) "($msg)" else ""}", msg)
    }

    /** WIM (dev = WIM を読む device ハンドル) のヘッダとイメージ一覧。子を 1 つ起こして、答えをもらって終わらせる。 */
    fun info(ctx: Context, dev: Long, mode: Mode = Mode.SERVICE): WimInfo {
        val control = acquire(ctx, mode)
        try {
            val timings = HashMap<String, Long>()
            val (fd, pid) = connect(ctx, control, true, 0, timings)
            val err = IntArray(1)
            val b = WimFuseNative.nativeInfo(fd, dev, err) ?: fail("wim info", err[0])
            return WimInfo.parse(String(b, Charsets.UTF_8))
        } finally { release(ctx, mode) }
    }

    /** image (1 始まり) を読み取り専用でマウントする。閉じる ([WimImage.close]) と子も終わる。dev は [WimImage] を閉じるまで開いておくこと。 */
    fun open(ctx: Context, dev: Long, image: Int, mode: Mode = Mode.SERVICE, pdeathsig: Boolean = true, cacheMb: Int = DEFAULT_CACHE_MB): WimImage {
        val control = acquire(ctx, mode)
        var ok = false
        try {
            val timings = LinkedHashMap<String, Long>()
            val (fd, pid) = connect(ctx, control, pdeathsig, cacheMb, timings)
            val t1 = SystemClock.elapsedRealtime()
            val err = IntArray(1)
            val h = WimFuseNative.nativeMount(fd, dev, image, err)
            if (h == 0L) { try { control.kill(pid) } catch (_: Exception) {}; fail("wim mount (image $image)", err[0]) }
            timings["mount_ms"] = SystemClock.elapsedRealtime() - t1
            val v = NtfsVolume.adoptHandle(h)
            ok = true
            return WimImage(v, pid, timings, control) { release(ctx, mode) }
        } finally { if (!ok) release(ctx, mode) }
    }
}
