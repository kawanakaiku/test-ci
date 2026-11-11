package org.usbmem.app

/**
 * 書き込み自己診断。マウント中の書き込み可ボリュームの上に、専用の一時フォルダ (usbmem_selftest_<時刻>) を作り、
 * 作成・書き込み・途中上書き・縮小・名前変更・移動・中身ごとの削除を実行して、毎回読み戻して検証する。
 * **触るのは自分で作ったフォルダの中だけ**。既存のファイルには一切触れない。終わると (失敗しても) 作ったものを全て消す。
 * 実機の USB ディスクでも、既存データを危険にさらさずに、書き込み経路 (ドライバ → USB) が正しく動くことを確かめるためのもの。
 */
object WriteSelfTest {
    data class Step(val name: String, val ok: Boolean, val ms: Long, val detail: String = "")
    data class Result(val steps: List<Step>, val folder: String, val cleaned: Boolean) {
        val passed: Boolean get() = steps.isNotEmpty() && steps.all { it.ok } && cleaned
        val failedStep: Step? get() = steps.firstOrNull { !it.ok }
    }

    private fun pattern(n: Int, seed: Int) = ByteArray(n) { (((it * 7) + seed) and 255).toByte() }

    private fun readAll(v: NtfsVolume, path: String): ByteArray {
        val size = v.stat(path).size.toInt()
        val out = ByteArray(size)
        var off = 0
        val buf = ByteArray(65536)
        while (off < size) {
            val n = v.read(path, off.toLong(), buf)
            if (n <= 0) break
            System.arraycopy(buf, 0, out, off, n); off += n
        }
        return if (off == size) out else out.copyOf(off)
    }

    private fun writeAll(v: NtfsVolume, path: String, offset: Long, data: ByteArray) {
        var done = 0
        while (done < data.size) {
            val chunk = data.copyOfRange(done, minOf(data.size, done + 65536))
            val n = v.write(path, offset + done, chunk, chunk.size)
            if (n <= 0) throw IllegalStateException("short write at ${offset + done}")
            done += n
        }
    }

    private fun exists(v: NtfsVolume, path: String): Boolean =
        try { v.stat(path); true } catch (e: NativeException) { if (e.code == 2) false else throw e }

    /** [now] は一時フォルダ名に使う文字列 (時刻)。volume は writable かつ createOnly=false であること。 */
    fun run(v: NtfsVolume, now: String, progress: (String) -> Unit = {}): Result {
        val steps = ArrayList<Step>()
        val root = "/usbmem_selftest_$now"
        fun step(name: String, body: () -> String?): Boolean {
            progress(name)
            val t0 = System.nanoTime()
            val r = try { body()?.let { false to it } ?: (true to "") } catch (e: Throwable) { false to (e.message ?: e.javaClass.simpleName) }
            steps += Step(name, r.first, (System.nanoTime() - t0) / 1_000_000, r.second)
            return r.first
        }
        fun check(cond: Boolean, msg: String): String? = if (cond) null else msg

        var cleaned = false
        var created = false   // 自分でフォルダを作れたときだけ後片付けをする (同名の既存フォルダを消さないため)
        try {
            if (!v.writable || v.createOnly) {
                steps += Step("precondition", false, 0, "ボリュームが書き込み可 (すべて許可) ではありません")
                return Result(steps, root, true)
            }
            if (exists(v, root)) { steps += Step("precondition", false, 0, "$root が既にあります (消しません)"); return Result(steps, root, true) }

            val a = pattern(300_000, 5)
            val b = pattern(4096, 77)
            var ok = step("フォルダを作る") { v.create("/", root.removePrefix("/"), true); created = true; check(v.stat(root).isDir, "not a directory") }
            ok = ok && step("ファイルを作って書く (300 KB)") {
                v.create(root, "f1.bin", false); writeAll(v, "$root/f1.bin", 0, a); v.sync()
                check(readAll(v, "$root/f1.bin").contentEquals(a), "read-back mismatch")
            }
            ok = ok && step("途中を上書きする (4 KB)") {
                writeAll(v, "$root/f1.bin", 100_000, b); v.sync()
                val exp = a.copyOf(); System.arraycopy(b, 0, exp, 100_000, b.size)
                val got = readAll(v, "$root/f1.bin")
                check(got.size == exp.size && got.contentEquals(exp), "mismatch after overwrite")
            }
            ok = ok && step("縮める (300 KB → 100 KB)") {
                v.truncate("$root/f1.bin", 100_000)
                val got = readAll(v, "$root/f1.bin")
                check(got.size == 100_000 && got.contentEquals(a.copyOf(100_000)), "mismatch after truncate (size ${got.size})")
            }
            ok = ok && step("名前を変える") {
                v.rename("$root/f1.bin", "f2.bin")
                check(!exists(v, "$root/f1.bin") && exists(v, "$root/f2.bin") && v.stat("$root/f2.bin").size == 100_000L, "rename result wrong")
            }
            ok = ok && step("別のフォルダへ移す") {
                v.create(root, "sub", true)
                v.move("$root/f2.bin", "$root/sub", "f2.bin")
                check(!exists(v, "$root/f2.bin") && readAll(v, "$root/sub/f2.bin").contentEquals(a.copyOf(100_000)), "move result wrong")
            }
            ok = ok && step("小さいファイルを 20 個作る") {
                for (i in 0 until 20) { v.create("$root/sub", "s$i.txt", false); writeAll(v, "$root/sub/s$i.txt", 0, pattern(100 + i * 37, i)) }
                v.sync()
                var bad: String? = null
                for (i in 0 until 20) if (!readAll(v, "$root/sub/s$i.txt").contentEquals(pattern(100 + i * 37, i))) bad = "s$i.txt mismatch"
                bad ?: check(v.list("$root/sub").size == 21, "entry count ${v.list("$root/sub").size} != 21")
            }
            if (!ok) progress("failed")
        } finally {
            // 失敗していても、自分が作ったものは必ず消す (中身ごと)。作れていなければ何も消さない
            if (created) {
                val t0 = System.nanoTime()
                cleaned = try { if (exists(v, root)) v.removeTree(root); !exists(v, root) } catch (e: Throwable) { false }
                steps += Step("後片付け (作ったものを全部消す)", cleaned, (System.nanoTime() - t0) / 1_000_000, if (cleaned) "" else "一時フォルダを消せませんでした: $root")
                runCatching { v.sync() }
            } else cleaned = true
        }
        return Result(steps, root, cleaned)
    }
}
