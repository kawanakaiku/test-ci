// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.File
import java.io.FileInputStream
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit

/**
 * kfsd の実行ファイル ([exe]) を起こして見張る。個人版の :kfsd サービス (libkfsd.so) と、別 APK のプラグインが同じものを使う。
 *
 * 起動は専用の長命スレッドで行う: 子は prctl(PR_SET_PDEATHSIG) を「起こしたスレッド」の死に結ぶので、使い捨てのスレッド (Binder のスレッドなど) から起こすと、
 * そのスレッドが終わった時に子が SIGKILL される。子の標準出力・標準エラーは読み続けて logcat (タグ kfsd-child) へ流す (読まないとパイプが詰まる)。終了は専用スレッドで waitpid する。
 */
class KfsdSpawner(private val exe: File) {
    class Child(val pid: Int) {
        val tail = ArrayDeque<String>()
        @Volatile var exit: Int? = null
    }

    private val children = ConcurrentHashMap<Int, Child>()
    private val thread = HandlerThread("kfsd-spawn").apply { start() }
    private val handler = Handler(thread.looper)

    /** [sock] (本体とつながった socketpair の端) を子の fd 3 へ渡して起こし、pid を返す。呼び出し側の [sock] は呼び出し側が閉じる。失敗は IOException (exec できない = ENOENT / EACCES など)。 */
    fun spawn(sock: ParcelFileDescriptor, extra: List<String> = emptyList()): Int {
        val task = FutureTask {
            val logFd = IntArray(1) { -1 }
            val pid = KfsdSpawnNative.nativeSpawn(exe.path, sock.fd, extra.toTypedArray(), logFd)
            if (pid < 0) throw java.io.IOException("exec ${exe.name} failed: errno ${-pid}")
            val c = Child(pid)
            children[pid] = c
            if (logFd[0] >= 0) {
                val pfd = ParcelFileDescriptor.adoptFd(logFd[0])
                Thread({
                    try {
                        FileInputStream(pfd.fileDescriptor).bufferedReader().forEachLine { l ->
                            Log.i("kfsd-child", "[$pid] $l")
                            synchronized(c.tail) { c.tail.addLast(l); while (c.tail.size > 60) c.tail.removeFirst() }
                        }
                    } catch (_: Exception) {} finally { try { pfd.close() } catch (_: Exception) {} }
                }, "kfsd-log-$pid").apply { isDaemon = true; start() }
            }
            Thread({ c.exit = KfsdSpawnNative.nativeWait(pid) }, "kfsd-wait-$pid").apply { isDaemon = true; start() }
            pid
        }
        handler.post(task)
        try {
            return task.get(15, TimeUnit.SECONDS)
        } catch (e: java.util.concurrent.ExecutionException) {
            throw (e.cause as? java.io.IOException) ?: java.io.IOException(e.cause?.toString() ?: e.toString())
        }
    }

    fun alive(pid: Int): Boolean = children[pid]?.let { it.exit == null } ?: false

    /** 終了していればその終了コード (シグナル死は 128 + シグナル)、生きていれば null。 */
    fun exitValue(pid: Int): Int? = children[pid]?.exit

    fun kill(pid: Int) { if (children[pid]?.exit == null) try { Os.kill(pid, OsConstants.SIGKILL) } catch (_: Exception) {} }

    /** 子の出力の最後の数十行 (診断用)。 */
    fun tail(pid: Int): String = children[pid]?.let { c -> synchronized(c.tail) { c.tail.joinToString("\n") } } ?: ""

    fun killAll() { for (pid in children.keys) kill(pid) }

    fun shutdown() { killAll(); thread.quitSafely() }

    /** `exe --version` を実行して出力を返す (exec が成立することの最小の確認)。 */
    fun version(): String {
        val nul = ParcelFileDescriptor.open(File("/dev/null"), ParcelFileDescriptor.MODE_READ_ONLY)
        try {
            val pid = spawn(nul, listOf("--version"))
            val t0 = System.nanoTime()
            while (alive(pid) && System.nanoTime() - t0 < 10_000_000_000L) Thread.sleep(20)
            Thread.sleep(50)   // ログを読むスレッドが最後の行を取り込むのを待つ
            return tail(pid).trim() + " (exit ${exitValue(pid)})"
        } finally { nul.close() }
    }
}
