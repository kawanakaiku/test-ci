package org.usbmem.app.wimfuse

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit

/**
 * libwimfuse.so (APK の nativeLibraryDir に展開された PIE の実行ファイル) を ProcessBuilder で exec して見張る。
 * :wimfuse サービスの中で使う (本線)。同じプロセスでも使える (試験・比較用の [WimFuse.Mode.DIRECT])。
 *
 * 起動は専用の長命スレッドで行う: 子は prctl(PR_SET_PDEATHSIG) を「起こしたスレッド」の死に結ぶので、使い捨てのスレッド (Binder のスレッドなど) から起こすと、
 * そのスレッドが終わった時に子が SIGKILL される。子の標準出力・標準エラーは読み続けて logcat (タグ wimfuse-child) へ流す (読まないとパイプが詰まる)。
 */
class ChildSpawner(private val exe: File) {
    class Child(val proc: Process, val pid: Int) {
        val tail = ArrayDeque<String>()
    }

    private val children = ConcurrentHashMap<Int, Child>()
    private val thread = HandlerThread("wimfuse-spawn").apply { start() }
    private val handler = Handler(thread.looper)

    /** 子を起こして pid を返す。socketName = 本体が待ち受けている抽象名前空間の UNIX ソケットの名前 (子が connect する)。失敗は IOException (exec できない = EACCES など)。 */
    fun spawn(socketName: String, pdeathsig: Boolean, cacheMb: Int = 0): Int {
        val task = FutureTask {
            val cmd = ArrayList<String>()
            cmd += exe.path; cmd += "--connect"; cmd += socketName
            if (pdeathsig) cmd += "--pdeathsig"
            if (cacheMb > 0) { cmd += "--cache-mb"; cmd += cacheMb.toString() }
            val p = ProcessBuilder(cmd).redirectErrorStream(true).start()
            val pid = pidOf(p)
            val c = Child(p, pid)
            children[pid] = c
            Thread({
                try {
                    p.inputStream.bufferedReader().forEachLine { l ->
                        Log.i("wimfuse-child", "[$pid] $l")
                        synchronized(c.tail) { c.tail.addLast(l); while (c.tail.size > 60) c.tail.removeFirst() }
                    }
                } catch (_: Exception) {}
            }, "wimfuse-log-$pid").apply { isDaemon = true; start() }
            pid
        }
        handler.post(task)
        try {
            return task.get(15, TimeUnit.SECONDS)
        } catch (e: java.util.concurrent.ExecutionException) {
            throw (e.cause as? java.io.IOException) ?: java.io.IOException(e.cause?.toString() ?: e.toString())
        }
    }

    fun alive(pid: Int): Boolean = children[pid]?.proc?.isAlive ?: false

    /** 終了していればその終了コード (シグナル死は 128 + シグナル)、生きていれば null。 */
    fun exitValue(pid: Int): Int? = children[pid]?.proc?.let { if (it.isAlive) null else it.exitValue() }

    fun kill(pid: Int) { children[pid]?.proc?.destroyForcibly() }

    /** 子の出力の最後の数十行 (診断用)。 */
    fun tail(pid: Int): String = children[pid]?.let { c -> synchronized(c.tail) { c.tail.joinToString("\n") } } ?: ""

    fun killAll() { for (c in children.values) c.proc.destroyForcibly() }

    fun shutdown() { killAll(); thread.quitSafely() }

    /** `libwimfuse.so --version` を実行して出力を返す (exec が成立することの最小の確認)。 */
    fun version(): String {
        val p = ProcessBuilder(exe.path, "--version").redirectErrorStream(true).start()
        val out = p.inputStream.bufferedReader().readText()
        if (!p.waitFor(10, TimeUnit.SECONDS)) { p.destroyForcibly(); throw java.io.IOException("--version timed out") }
        return out.trim() + " (exit ${p.exitValue()})"
    }

    private fun pidOf(p: Process): Int =
        Regex("pid=(\\d+)").find(p.toString())?.groupValues?.get(1)?.toInt()   // Android の Process.toString は "Process[pid=123, hasExited=false]"
            ?: try { (p.javaClass.getMethod("pid").invoke(p) as Number).toInt() } catch (e: Exception) { -1 }
}
