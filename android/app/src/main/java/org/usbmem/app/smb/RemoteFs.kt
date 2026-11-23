// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import java.io.InputStream
import java.io.OutputStream

/** リモートのファイルシステムの 1 項目。[mtimeMs] = 更新時刻 (エポックからのミリ秒、不明なら 0)。 */
data class RemoteEntry(val name: String, val isDir: Boolean, val size: Long, val mtimeMs: Long)

/** 取り消しの旗。長い転送・一覧の合間に [check] を呼ぶ。別スレッドの [cancel] が、次の [check] を [SmbCancelled] にする。 */
class SmbCancel {
    @Volatile var cancelled = false
        private set
    private var cancelHook: (() -> Unit)? = null

    fun cancel() {
        val f: (() -> Unit)?
        synchronized(this) { cancelled = true; f = cancelHook }
        try { f?.invoke() } catch (_: Exception) { }
    }

    /** 取り消されたら呼ぶ後始末 (接続の途中の接続を閉じる、など) を 1 つ登録する。登録した時点で取り消し済みなら、すぐ呼ぶ。 */
    fun onCancel(f: () -> Unit) {
        val now: Boolean
        synchronized(this) { cancelHook = f; now = cancelled }
        if (now) try { f() } catch (_: Exception) { }
    }

    fun check() { if (cancelled) throw SmbCancelled() }
}

/** 進捗の通知 ([total] が分からなければ -1)。転送のスレッドから呼ばれる。 */
fun interface SmbProgress { fun onProgress(done: Long, total: Long) }

/** 開いたファイルの読み取り (ストリームではなく位置指定: SAF の ProxyFileDescriptor に合う) */
interface RemoteReader : AutoCloseable {
    val size: Long
    /** [offset] から最大 [len] バイトを [buf] の [off] へ読む。読めたバイト数。0 = ファイルの終わり (要求した [len] が 0 でなければ、読めるだけ読むまで繰り返す)。 */
    fun read(offset: Long, buf: ByteArray, off: Int, len: Int): Int
}

/** 開いたファイルへの書き込み */
interface RemoteWriter : AutoCloseable {
    /** [offset] へ [len] バイトを書く (全部書けるまで繰り返す)。ファイルの終わりを超えれば伸びる。 */
    fun write(offset: Long, buf: ByteArray, off: Int, len: Int)
    fun setLength(length: Long)
    /** サーバー側のバッファをディスクへ書き出させる */
    fun flush()
}

/**
 * リモートのファイルシステムの操作 (ネットワーク共有 1 つ分)。パスは "/" 区切りで、ルートが "/"。".." と "." は拒否する。
 * 失敗は [SmbException] (種類つき)。接続が切れたら、以後の操作はすべて [SmbErrorKind.Disconnected] (自動の再接続はしない: パスワードをメモリに残さないため)。
 *
 * USB のボリューム ([org.usbmem.app.NtfsVolume]) はネイティブの FsVolume を JNI で呼ぶので、これとは別のまま (docs/smb.md 5 章)。SAF のプロバイダが両方を公開する。
 */
interface RemoteFs : AutoCloseable {
    /** 接続が生きているか (切れたと分かったら false) */
    val isAlive: Boolean
    /** ネゴシエートした方言 (例: "SMB 3.1.1") */
    val dialect: String
    /** 暗号化されているか */
    val encrypted: Boolean
    /** 署名されているか */
    val signed: Boolean
    /** サーバーが、この共有への書き込みを許すか (接続時に分かる最大のアクセス権から) */
    val serverAllowsWrite: Boolean

    fun stat(path: String): RemoteEntry
    fun list(path: String): List<RemoteEntry>
    fun exists(path: String): Boolean = try { stat(path); true } catch (e: SmbException) { if (e.kind == SmbErrorKind.NotFound) false else throw e }
    fun openRead(path: String): RemoteReader
    /** [create] = 無ければ作る (有れば失敗しない)、[truncate] = 開くとき長さ 0 にする、[readToo] = 読み取りも許す */
    fun openWrite(path: String, create: Boolean, truncate: Boolean, readToo: Boolean = false): RemoteWriter
    /** 新しい空のファイルを作る。同名があれば [SmbErrorKind.Exists] */
    fun createFile(path: String)
    fun mkdir(path: String)
    /** ファイルまたはフォルダ (中身ごと) を消す */
    fun delete(path: String)
    /** [from] を [to] へ名前変更・移動する (同じ共有の中。[to] が有れば [SmbErrorKind.Exists]) */
    fun rename(from: String, to: String)

    /** [path] の中身を [out] へ流す (全部をメモリに載せない)。コピーしたバイト数 */
    fun copyTo(path: String, out: OutputStream, cancel: SmbCancel? = null, progress: SmbProgress? = null): Long {
        openRead(path).use { r ->
            val total = r.size
            val buf = ByteArray(COPY_BUFFER)
            var off = 0L
            while (true) {
                cancel?.check()
                val n = r.read(off, buf, 0, buf.size)
                if (n <= 0) break
                out.write(buf, 0, n)
                off += n
                progress?.onProgress(off, total)
            }
            return off
        }
    }

    /** [input] の中身を [path] へ書く (全部をメモリに載せない)。[truncate] = 既存の中身を捨てる。書いたバイト数 */
    fun copyFrom(path: String, input: InputStream, cancel: SmbCancel? = null, progress: SmbProgress? = null, truncate: Boolean = true, total: Long = -1): Long {
        openWrite(path, create = true, truncate = truncate).use { w ->
            val buf = ByteArray(COPY_BUFFER)
            var off = 0L
            while (true) {
                cancel?.check()
                val n = input.read(buf)
                if (n < 0) break
                if (n == 0) continue
                w.write(off, buf, 0, n)
                off += n
                progress?.onProgress(off, total)
            }
            w.flush()
            return off
        }
    }

    companion object { const val COPY_BUFFER = 1 shl 20 }
}

/** パスの変換・検査 (SMBJ に依存しない) */
object SmbPaths {
    /** "/a/b" → ["a", "b"]。"." ".." と空でない区切り以外の制御文字・SMB の禁止文字を含む要素は [SmbException] (InvalidName) */
    fun parts(path: String): List<String> {
        val l = path.split('/').filter { it.isNotEmpty() }
        for (p in l) if (p == "." || p == ".." || !validName(p)) throw SmbException(SmbErrorKind.InvalidName)
        return l
    }

    /** SMB のパス ("a\\b"、ルートは空文字) */
    fun toSmb(path: String): String = parts(path).joinToString("\\")

    /** 要素の名前として使えるか (区切り・制御文字・Windows で使えない文字を含まない) */
    fun validName(n: String): Boolean {
        if (n.isEmpty() || n.length > 255 || n == "." || n == "..") return false
        if (n.any { it.code < 0x20 || it in "\\/:*?\"<>|" }) return false
        return true
    }

    fun parent(path: String): String {
        val l = path.split('/').filter { it.isNotEmpty() }
        return "/" + l.dropLast(1).joinToString("/")
    }

    fun name(path: String): String = path.split('/').filter { it.isNotEmpty() }.lastOrNull() ?: ""

    fun child(parent: String, name: String): String = if (parent.endsWith("/")) parent + name else "$parent/$name"
}
