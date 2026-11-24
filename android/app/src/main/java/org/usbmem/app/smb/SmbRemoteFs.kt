// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import com.hierynomus.msdtyp.AccessMask
import com.hierynomus.msfscc.FileAttributes
import com.hierynomus.mssmb2.SMB2CreateDisposition
import com.hierynomus.mssmb2.SMB2CreateOptions
import com.hierynomus.mssmb2.SMB2Dialect
import com.hierynomus.mssmb2.SMB2ShareAccess
import com.hierynomus.mssmb2.SMBApiException
import com.hierynomus.smbj.SMBClient
import com.hierynomus.smbj.SmbConfig
import com.hierynomus.smbj.auth.AuthenticationContext
import com.hierynomus.smbj.auth.NtlmAuthenticator
import com.hierynomus.smbj.connection.Connection
import com.hierynomus.smbj.session.Session
import com.hierynomus.smbj.share.DiskShare
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.util.EnumSet
import java.util.concurrent.TimeUnit
import javax.net.SocketFactory
import com.hierynomus.smbj.share.File as SmbFile

/**
 * 接続のソケット: TCP keepalive と TCP_NODELAY を付け、**接続にも時間の上限を付ける** (SMBJ の既定は無期限で、届かないホストでは OS 任せの 2 分ほど待つ)。
 * 読み取りのタイムアウト (SO_TIMEOUT) は付けない: SMBJ の読み取りスレッドが、何も流れていない接続を「切れた」と見なしてしまうため。
 * 応答が来ないことの検出は、SMBJ の要求ごとのタイムアウト ([SmbRemoteFs.connect] の timeoutSec) が担う。
 */
internal class SmbSocketFactory(private val connectTimeoutMs: Int, private val socketBuffer: Int = 0) : SocketFactory() {
    /** 作ったソケット。取り消し・切断のとき、SMBJ の内部の待ち (ネゴシエーション中など。SMBJ 自身は閉じられない) を、ソケットを閉じて起こすために持つ */
    private val sockets = java.util.concurrent.CopyOnWriteArrayList<Socket>()
    private fun tune(s: Socket): Socket {
        s.keepAlive = true; s.tcpNoDelay = true
        // 送受信バッファ (接続の前に指定する。OS の上限までに丸められる。0 = OS の既定)。遅延の大きい回線で、1 度に飛ばせる量が増える
        if (socketBuffer > 0) try { s.receiveBufferSize = socketBuffer; s.sendBufferSize = socketBuffer } catch (_: Exception) { }
        sockets.add(s); return s
    }

    /** 全部閉じる (ブロック中の接続・読み書きは例外で戻る。SMBJ の読み取りスレッドが失敗を待ち手へ伝える) */
    fun closeAll() { for (s in sockets) try { s.close() } catch (_: Exception) { }; sockets.clear() }

    private fun open(addr: InetSocketAddress, local: InetSocketAddress?): Socket {
        val s = tune(Socket())
        try {
            if (local != null) s.bind(local)
            s.connect(addr, connectTimeoutMs)
        } catch (e: Throwable) {
            try { s.close() } catch (_: Exception) { }
            throw e
        }
        return s
    }

    override fun createSocket(): Socket = tune(Socket())
    override fun createSocket(host: String, port: Int): Socket = open(InetSocketAddress(host, port), null)
    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket = open(InetSocketAddress(host, port), InetSocketAddress(localHost, localPort))
    override fun createSocket(host: InetAddress, port: Int): Socket = open(InetSocketAddress(host, port), null)
    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket = open(InetSocketAddress(address, port), InetSocketAddress(localAddress, localPort))
}

/**
 * 転送の調整 (docs/smb.md 13 章)。既定値は、CI の測定 (SmbTest.transferSpeedMatrix) で選んだもの。試験は値を変えて速さを比べる。
 * どの値でも、通信の内容 (SMB のメッセージ) の意味は変わらない。
 */
data class SmbTuning(
    /** 1 要求の最大の大きさ (読み・書き・ディレクトリの一覧)。サーバーが許す上限 (MaxRead / MaxWrite / MaxTransact) を超えない */
    val bufferSize: Int = 1 shl 20,
    /** 順番に読んでいるとき、いまの要求の応答を待つ間に、次の要求を先に出す (読み取りの先読み)。ランダムなアクセスでは働かない */
    val readAhead: Boolean = true,
    /** 署名・暗号化・ハッシュを、端末の JCE (ネイティブ) で行う。BouncyCastle と結果が一致した部品だけ ([HybridSecurityProvider]) */
    val nativeCrypto: Boolean = true,
    /** TCP の送受信バッファ (バイト。0 = OS の既定) */
    val socketBuffer: Int = 0,
)

/**
 * [RemoteFs] の SMB 実装 (SMBJ。SMB 2.x / 3.x のみ。SMB 1 は実装しない)。1 つの共有 (DiskShare) への 1 つの接続・1 つのセッション。
 *
 * - **パスワード**: [connect] の引数の配列は、認証が済んだらすぐゼロで埋める (成功・失敗・取り消しのどれでも)。接続後は持たない。
 *   なので、接続が切れたあとの**自動再接続はしない** ([RemoteFs])。
 * - 認証は NTLM だけ (SMBJ の SPNEGO / Kerberos は JGSS を使うが、Android には無い)。
 * - 例外はすべて [SmbException] にして投げる (メッセージにパス・ユーザー名・サーバーの文字列を含めない)。接続が壊れたと分かったら、以後 [isAlive] は false。
 * - 並行: SMBJ の接続は複数スレッドから使える。ファイルのハンドル ([RemoteReader] / [RemoteWriter]) は 1 スレッドずつ。
 */
class SmbRemoteFs private constructor(
    private val client: SMBClient,
    private val connection: Connection,
    private val session: Session,
    private val share: DiskShare,
    private val sockets: SmbSocketFactory,
    private val chunk: Int,
    private val tuning: SmbTuning,
    override val dialect: String,
    override val encrypted: Boolean,
    override val signed: Boolean,
    override val serverAllowsWrite: Boolean,
) : RemoteFs {
    @Volatile private var closed = false
    @Volatile private var lost = false

    override val isAlive: Boolean
        get() = !closed && !lost && (try { connection.isConnected && share.isConnected } catch (_: Exception) { false })

    private fun fail(e: Throwable): SmbException {
        val m = SmbErrors.map(e)
        if (SmbErrors.isConnectionLost(m.kind)) lost = true
        return m
    }

    private inline fun <T> guard(block: () -> T): T {
        if (closed) throw SmbException(SmbErrorKind.Disconnected, detail = "closed")
        if (lost) throw SmbException(SmbErrorKind.Disconnected, detail = "lost")
        try { return block() } catch (e: Throwable) { throw fail(e) }
    }

    private fun entryOf(info: com.hierynomus.msfscc.fileinformation.FileIdBothDirectoryInformation): RemoteEntry =
        RemoteEntry(info.fileName, (info.fileAttributes and FileAttributes.FILE_ATTRIBUTE_DIRECTORY.value) != 0L, info.endOfFile, mtime(info.lastWriteTime))

    private fun mtime(t: com.hierynomus.msdtyp.FileTime?): Long = try { t?.toEpochMillis() ?: 0L } catch (_: Exception) { 0L }

    override fun stat(path: String): RemoteEntry = guard {
        val p = SmbPaths.toSmb(path)
        if (p.isEmpty()) return@guard RemoteEntry("", true, 0L, 0L)
        val fi = share.getFileInformation(p)
        RemoteEntry(SmbPaths.name(path), fi.standardInformation.isDirectory, fi.standardInformation.endOfFile, mtime(fi.basicInformation.lastWriteTime))
    }

    override fun list(path: String): List<RemoteEntry> = guard {
        val p = SmbPaths.toSmb(path)
        share.list(p).filter { it.fileName != "." && it.fileName != ".." }.map { entryOf(it) }
    }

    private inner class Reader(private val f: SmbFile, override val size: Long, private val aheadAllowed: Boolean) : RemoteReader {
        /** 順番に読んでいるときの先読み (SMBJ の入力ストリーム = 次の 1 要求を、いまの分を使っている間に先に出す)。位置がずれたら捨てる */
        private var stream: java.io.InputStream? = null
        private var streamPos = -1L
        private var lastEnd = -1L

        private fun closeStream() { try { stream?.close() } catch (_: Exception) { }; stream = null; streamPos = -1L }

        private fun readDirect(offset: Long, buf: ByteArray, off: Int, len: Int): Int {
            var done = 0
            while (done < len) {
                val want = minOf(len - done, chunk)
                val n = try { f.read(buf, offset + done, off + done, want) } catch (e: SMBApiException) { if (e.statusCode == SmbErrors.STATUS_END_OF_FILE) -1 else throw e }
                if (n <= 0) break
                done += n
            }
            return done
        }

        private fun readStream(offset: Long, buf: ByteArray, off: Int, len: Int): Int {
            try {
                if (stream == null && offset >= size) return 0
                val s: java.io.InputStream = stream ?: run {
                    val ns: java.io.InputStream = f.inputStream
                    ns.skip(offset)   // まだ何も読んでいないストリームでは、次に要求する位置を進めるだけ (通信しない)
                    stream = ns; streamPos = offset
                    ns
                }
                var done = 0
                while (done < len) {
                    val n = s.read(buf, off + done, len - done)
                    if (n <= 0) break   // -1 = ファイルの終わり
                    done += n
                }
                streamPos += done
                return done
            } catch (e: Throwable) { closeStream(); throw e }
        }

        override fun read(offset: Long, buf: ByteArray, off: Int, len: Int): Int = guard {
            // 直前の続きの位置 (2 回続けて順番に読まれたとき) から先読みを始める。ランダムなアクセス (先頭と末尾だけ読むなど) では先読みしない
            val sequential = aheadAllowed && tuning.readAhead && (offset == lastEnd || (stream != null && offset == streamPos))
            val n = if (sequential) readStream(offset, buf, off, len) else { closeStream(); readDirect(offset, buf, off, len) }
            lastEnd = offset + n
            n
        }
        override fun close() { closeStream(); try { f.close() } catch (_: Exception) { } }
    }

    private inner class Writer(private val f: SmbFile) : RemoteWriter {
        override fun write(offset: Long, buf: ByteArray, off: Int, len: Int) = guard {
            var done = 0
            while (done < len) {
                val want = minOf(len - done, chunk)
                val n = f.write(buf, offset + done, off + done, want).toInt()
                if (n <= 0) throw SmbException(SmbErrorKind.Other, detail = "short write")
                done += n
            }
        }
        override fun setLength(length: Long) = guard { f.setLength(length) }
        override fun flush() = guard { f.flush() }
        override fun close() { try { f.close() } catch (_: Exception) { } }
    }

    private fun attrs() = EnumSet.of(FileAttributes.FILE_ATTRIBUTE_NORMAL)
    private fun fileOpts() = EnumSet.of(SMB2CreateOptions.FILE_NON_DIRECTORY_FILE)

    override fun openRead(path: String): RemoteReader = openRead(path, true)

    override fun openRead(path: String, readAhead: Boolean): RemoteReader = guard {
        val f = share.openFile(SmbPaths.toSmb(path), EnumSet.of(AccessMask.GENERIC_READ), attrs(), SMB2ShareAccess.ALL, SMB2CreateDisposition.FILE_OPEN, fileOpts())
        try { Reader(f, f.fileInformation.standardInformation.endOfFile, readAhead) } catch (e: Throwable) { try { f.close() } catch (_: Exception) { }; throw e }
    }

    override fun openWrite(path: String, create: Boolean, truncate: Boolean, readToo: Boolean): RemoteWriter = guard {
        val disp = when {
            create && truncate -> SMB2CreateDisposition.FILE_OVERWRITE_IF
            create -> SMB2CreateDisposition.FILE_OPEN_IF
            truncate -> SMB2CreateDisposition.FILE_OVERWRITE
            else -> SMB2CreateDisposition.FILE_OPEN
        }
        val access = if (readToo) EnumSet.of(AccessMask.GENERIC_WRITE, AccessMask.GENERIC_READ) else EnumSet.of(AccessMask.GENERIC_WRITE)
        Writer(share.openFile(SmbPaths.toSmb(path), access, attrs(), SMB2ShareAccess.ALL, disp, fileOpts()))
    }

    override fun createFile(path: String) {
        guard {
            val p = SmbPaths.toSmb(path)
            if (p.isEmpty()) throw SmbException(SmbErrorKind.InvalidName)
            share.openFile(p, EnumSet.of(AccessMask.GENERIC_WRITE), attrs(), SMB2ShareAccess.ALL, SMB2CreateDisposition.FILE_CREATE, fileOpts()).close()
        }
    }

    override fun mkdir(path: String) {
        guard {
            val p = SmbPaths.toSmb(path)
            if (p.isEmpty()) throw SmbException(SmbErrorKind.Exists)
            share.mkdir(p)
        }
    }

    override fun delete(path: String) {
        guard {
            val p = SmbPaths.toSmb(path)
            if (p.isEmpty()) throw SmbException(SmbErrorKind.AccessDenied)   // 共有のルートは消せない
            if (share.getFileInformation(p).standardInformation.isDirectory) share.rmdir(p, true) else share.rm(p)
        }
    }

    override fun rename(from: String, to: String) {
        guard {
            val a = SmbPaths.toSmb(from); val b = SmbPaths.toSmb(to)
            if (a.isEmpty() || b.isEmpty()) throw SmbException(SmbErrorKind.AccessDenied)
            val e = share.open(a, EnumSet.of(AccessMask.DELETE), EnumSet.of(FileAttributes.FILE_ATTRIBUTE_NORMAL), SMB2ShareAccess.ALL, SMB2CreateDisposition.FILE_OPEN, EnumSet.noneOf(SMB2CreateOptions::class.java))
            try { e.rename(b, false) } finally { try { e.close() } catch (_: Exception) { } }
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        val force = lost
        if (force) sockets.closeAll()   // 切れた接続への LOGOFF 等で、時間切れまで待たされない
        if (!force) try { share.close() } catch (_: Exception) { }
        if (!force) try { session.close() } catch (_: Exception) { }
        try { if (force) connection.close(true) else connection.close() } catch (_: Exception) { }
        try { client.close() } catch (_: Exception) { }
    }

    companion object {
        const val DEFAULT_TIMEOUT_SEC = 30
        private const val CONNECT_TIMEOUT_MS = 10_000

        internal fun dialectsFor(min: SmbMinVersion): List<SMB2Dialect> = when (min) {
            SmbMinVersion.V202 -> listOf(SMB2Dialect.SMB_2_0_2, SMB2Dialect.SMB_2_1, SMB2Dialect.SMB_3_0, SMB2Dialect.SMB_3_0_2, SMB2Dialect.SMB_3_1_1)
            SmbMinVersion.V21 -> listOf(SMB2Dialect.SMB_2_1, SMB2Dialect.SMB_3_0, SMB2Dialect.SMB_3_0_2, SMB2Dialect.SMB_3_1_1)
            SmbMinVersion.V30 -> listOf(SMB2Dialect.SMB_3_0, SMB2Dialect.SMB_3_0_2, SMB2Dialect.SMB_3_1_1)
            SmbMinVersion.V311 -> listOf(SMB2Dialect.SMB_3_1_1)
        }

        internal fun dialectName(d: SMB2Dialect?): String = when (d) {
            SMB2Dialect.SMB_2_0_2 -> "SMB 2.0.2"
            SMB2Dialect.SMB_2_1 -> "SMB 2.1"
            SMB2Dialect.SMB_3_0 -> "SMB 3.0"
            SMB2Dialect.SMB_3_0_2 -> "SMB 3.0.2"
            SMB2Dialect.SMB_3_1_1 -> "SMB 3.1.1"
            else -> "SMB 2/3"
        }

        private fun wipe(a: CharArray?) { if (a != null) java.util.Arrays.fill(a, '\u0000') }

        /**
         * [cfg] の共有へ接続して認証する。[password] (ゲストなら空) は、**この関数が戻る前にゼロで埋める** (呼び出し側で別に持っているコピーは、呼び出し側が消す)。
         * [cancel] を取り消すと、通信中の接続を閉じて [SmbErrorKind.Cancelled] で失敗する。[timeoutSec] = 要求 1 つごとの応答の上限 (読み・書き・その他)。
         * ネットワークの操作なので、メインスレッドから呼ばない。
         */
        fun connect(cfg: SmbConnection, password: CharArray, cancel: SmbCancel? = null, timeoutSec: Int = DEFAULT_TIMEOUT_SEC, tuning: SmbTuning = SmbTuning()): SmbRemoteFs {
            val sockets = SmbSocketFactory(CONNECT_TIMEOUT_MS, tuning.socketBuffer)
            val config = SmbConfig.builder()
                .withDialects(dialectsFor(cfg.minVersion))
                .withMultiProtocolNegotiate(false)
                .withAuthenticators(NtlmAuthenticator.Factory())
                .withDfsEnabled(false)
                .withSigningRequired(cfg.requireSigning)
                .withEncryptData(cfg.requireEncryption)
                .withBufferSize(tuning.bufferSize)
                .withSecurityProvider(if (tuning.nativeCrypto) HybridSecurityProvider.get() else HybridSecurityProvider.bcOnly())
                .withTimeout(timeoutSec.toLong(), TimeUnit.SECONDS)
                .withSocketFactory(sockets)
                // ディレクトリのリース (SMB 3 のキャッシュ): 有効だと 2 回目以降の一覧がサーバーに問い合わせずにキャッシュから返り、
                // 切断・無応答を一覧で検出できず、他のクライアントの変更も遅れて見えうる。常に最新を取る (リースを使わない)
                .withDirectoryLeasingEnabled(false)
                .build()
            val client = SMBClient(config)
            // 取り消し: ソケットを閉じ (SMBJ の接続はネゴシエーション中だと client.close() では届かない)、接続中のスレッドを起こす
            val worker = Thread.currentThread()
            val connecting = java.util.concurrent.atomic.AtomicBoolean(true)
            cancel?.onCancel {
                sockets.closeAll()
                if (connecting.get()) worker.interrupt()
                client.close()
            }
            var conn: Connection? = null
            var ses: Session? = null
            var disk: DiskShare? = null
            val ac = if (cfg.isGuest) AuthenticationContext.guest() else AuthenticationContext(cfg.user, password, cfg.domain)
            try {
                cancel?.check()
                val c = client.connect(cfg.host, cfg.port)
                conn = c
                cancel?.check()
                val s = c.authenticate(ac)
                ses = s
                wipe(password)   // 認証が済んだ: これ以降はパスワードを使わない
                val sh = s.connectShare(cfg.share)
                val d = sh as? DiskShare
                if (d == null) { try { sh.close() } catch (_: Exception) { }; throw SmbException(SmbErrorKind.ShareNotFound, detail = "not a disk share") }
                disk = d
                cancel?.check()
                val np = c.negotiatedProtocol
                val maxAccess = try { d.treeConnect.maximalAccess } catch (_: Exception) { null }
                val writeOk = maxAccess == null || maxAccess.isEmpty() || maxAccess.contains(AccessMask.FILE_WRITE_DATA) || maxAccess.contains(AccessMask.FILE_ADD_FILE) ||
                    maxAccess.contains(AccessMask.GENERIC_WRITE) || maxAccess.contains(AccessMask.GENERIC_ALL)
                return SmbRemoteFs(
                    client, c, s, d, sockets, minOf(tuning.bufferSize, np.maxReadSize.coerceAtLeast(4096), np.maxWriteSize.coerceAtLeast(4096)), tuning,
                    dialectName(np.dialect), s.shouldEncryptData(), s.isSigningRequired, writeOk,
                )
            } catch (e: Throwable) {
                sockets.closeAll()   // 失敗した接続の後始末 (LOGOFF など) で待たされない
                try { disk?.close() } catch (_: Exception) { }
                try { ses?.close() } catch (_: Exception) { }
                try { conn?.close(true) } catch (_: Exception) { }
                try { client.close() } catch (_: Exception) { }
                if (cancel?.cancelled == true) throw SmbException(SmbErrorKind.Cancelled)
                throw SmbErrors.map(e, connecting = true)
            } finally {
                connecting.set(false)
                if (cancel?.cancelled == true) Thread.interrupted()   // 取り消しの割り込みを、呼び出し元のスレッドに残さない
                wipe(password)
                // AuthenticationContext が持つ配列 (コピーかもしれない) も消す
                try { wipe(ac.password) } catch (_: Exception) { }
            }
        }
    }
}
