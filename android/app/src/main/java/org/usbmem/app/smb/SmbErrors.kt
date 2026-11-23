// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import com.hierynomus.mssmb2.SMBApiException
import com.hierynomus.protocol.transport.TransportException
import java.io.EOFException
import java.io.IOException
import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.NoRouteToHostException
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.nio.channels.ClosedChannelException
import java.util.concurrent.TimeoutException

/** 失敗の種類。画面と SAF が、これで文言・errno を決める (サーバー・SMBJ の例外のメッセージは使わない) */
enum class SmbErrorKind {
    /** ユーザー名・パスワードが違う / アカウントが無効・期限切れ・ロックアウト */
    Auth,
    AccessDenied,
    NotFound,
    /** 共有名が無い */
    ShareNotFound,
    Exists,
    NotEmpty,
    /** 接続できなかった (名前が引けない・拒否・経路なし・接続のタイムアウト) */
    Network,
    /** 接続できたあとに切れた (相手が閉じた・リセット・EOF) */
    Disconnected,
    /** 要求にサーバーが答えない (応答のタイムアウト) */
    Timeout,
    NoSpace,
    Busy,
    /** バージョン・暗号化・署名の条件が合わない / SMB のメッセージが不正 */
    Protocol,
    InvalidName,
    Cancelled,
    ReadOnly,
    Other,
}

/**
 * SMB の操作の失敗。[message] は固定の短い英文 + 種類 + NT ステータス (16 進) だけ: **パスワードはもちろん、ユーザー名・パス・サーバーが返した文字列も含めない**
 * (例外のメッセージは、ログ・クラッシュレポートに出ることがあるため)。原因の例外 (cause) もつながない。
 */
class SmbException(val kind: SmbErrorKind, val status: Long = 0L, detail: String = "") :
    IOException("smb " + kind.name + (if (status != 0L) " (0x" + java.lang.Long.toHexString(status) + ")" else "") + (if (detail.isNotEmpty()) " " + detail else ""))

/** 例外 → [SmbException]。NT ステータスの値は MS-ERREF。 */
object SmbErrors {
    const val STATUS_NOT_SUPPORTED = 0xC00000BBL
    const val STATUS_INVALID_PARAMETER = 0xC000000DL
    const val STATUS_NO_SUCH_FILE = 0xC000000FL
    const val STATUS_END_OF_FILE = 0xC0000011L
    const val STATUS_ACCESS_DENIED = 0xC0000022L
    const val STATUS_OBJECT_NAME_INVALID = 0xC0000033L
    const val STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034L
    const val STATUS_OBJECT_NAME_COLLISION = 0xC0000035L
    const val STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003AL
    const val STATUS_SHARING_VIOLATION = 0xC0000043L
    const val STATUS_LOGON_FAILURE = 0xC000006DL
    const val STATUS_ACCOUNT_RESTRICTION = 0xC000006EL
    const val STATUS_INVALID_LOGON_HOURS = 0xC000006FL
    const val STATUS_INVALID_WORKSTATION = 0xC0000070L
    const val STATUS_PASSWORD_EXPIRED = 0xC0000071L
    const val STATUS_ACCOUNT_DISABLED = 0xC0000072L
    const val STATUS_DISK_FULL = 0xC000007FL
    const val STATUS_FILE_IS_A_DIRECTORY = 0xC00000BAL
    const val STATUS_BAD_NETWORK_NAME = 0xC00000CCL
    const val STATUS_DIRECTORY_NOT_EMPTY = 0xC0000101L
    const val STATUS_NOT_A_DIRECTORY = 0xC0000103L
    const val STATUS_ACCOUNT_LOCKED_OUT = 0xC0000234L
    const val STATUS_LOGON_TYPE_NOT_GRANTED = 0xC000015BL
    const val STATUS_USER_SESSION_DELETED = 0xC0000203L
    const val STATUS_NETWORK_NAME_DELETED = 0xC00000C9L
    const val STATUS_CONNECTION_DISCONNECTED = 0xC000020CL
    const val STATUS_MEDIA_WRITE_PROTECTED = 0xC00000A2L
    const val STATUS_CANNOT_DELETE = 0xC0000121L
    const val STATUS_INSUFFICIENT_RESOURCES = 0xC000009AL

    /** NT ステータス → 種類 (SMBJ に依存しない部分。単体試験できる) */
    fun kindOfStatus(s: Long): SmbErrorKind = when (s) {
        STATUS_LOGON_FAILURE, STATUS_ACCOUNT_RESTRICTION, STATUS_INVALID_LOGON_HOURS, STATUS_INVALID_WORKSTATION, STATUS_PASSWORD_EXPIRED,
        STATUS_ACCOUNT_DISABLED, STATUS_ACCOUNT_LOCKED_OUT, STATUS_LOGON_TYPE_NOT_GRANTED -> SmbErrorKind.Auth
        STATUS_ACCESS_DENIED, STATUS_CANNOT_DELETE -> SmbErrorKind.AccessDenied
        STATUS_MEDIA_WRITE_PROTECTED -> SmbErrorKind.ReadOnly
        STATUS_NO_SUCH_FILE, STATUS_OBJECT_NAME_NOT_FOUND, STATUS_OBJECT_PATH_NOT_FOUND -> SmbErrorKind.NotFound
        STATUS_BAD_NETWORK_NAME -> SmbErrorKind.ShareNotFound
        STATUS_OBJECT_NAME_COLLISION -> SmbErrorKind.Exists
        STATUS_DIRECTORY_NOT_EMPTY -> SmbErrorKind.NotEmpty
        STATUS_DISK_FULL -> SmbErrorKind.NoSpace
        STATUS_SHARING_VIOLATION -> SmbErrorKind.Busy
        STATUS_OBJECT_NAME_INVALID -> SmbErrorKind.InvalidName
        STATUS_NOT_SUPPORTED, STATUS_INVALID_PARAMETER -> SmbErrorKind.Protocol
        STATUS_USER_SESSION_DELETED, STATUS_NETWORK_NAME_DELETED, STATUS_CONNECTION_DISCONNECTED -> SmbErrorKind.Disconnected
        else -> SmbErrorKind.Other
    }

    /** 原因の連鎖にある、見分けのつく例外を探す */
    private fun chain(e: Throwable): Sequence<Throwable> = generateSequence(e) { if (it.cause === it) null else it.cause }.take(12)

    /**
     * 例外を [SmbException] にする。[connecting] = 接続・認証・共有への接続の途中 (ここで通信が壊れたら [SmbErrorKind.Network]。接続できたあとなら [SmbErrorKind.Disconnected])。
     * 利用者の取り消し (スレッドの割り込み・[SmbCancelled]) は [SmbErrorKind.Cancelled]。
     */
    fun map(e: Throwable, connecting: Boolean = false): SmbException {
        if (e is SmbException) return e
        for (t in chain(e)) {
            if (t is SmbException) return t
            if (t is SMBApiException) {
                val k = kindOfStatus(t.statusCode)
                return SmbException(if (connecting && k == SmbErrorKind.Disconnected) SmbErrorKind.Network else k, t.statusCode)
            }
        }
        for (t in chain(e)) {
            when (t) {
                is SmbCancelled, is InterruptedException, is java.nio.channels.ClosedByInterruptException -> return SmbException(SmbErrorKind.Cancelled)
                is UnknownHostException, is ConnectException, is NoRouteToHostException -> return SmbException(SmbErrorKind.Network, detail = t.javaClass.simpleName)
                is SocketTimeoutException, is TimeoutException ->
                    return SmbException(if (connecting) SmbErrorKind.Network else SmbErrorKind.Timeout, detail = t.javaClass.simpleName)
                is InterruptedIOException -> return SmbException(SmbErrorKind.Timeout, detail = t.javaClass.simpleName)
                is SocketException, is EOFException, is ClosedChannelException ->
                    return SmbException(if (connecting) SmbErrorKind.Network else SmbErrorKind.Disconnected, detail = t.javaClass.simpleName)
            }
        }
        // SMBJ の TransportException (切断・送受信の失敗・ネゴシエーション失敗) / SMBRuntimeException (プロトコルの不一致)
        if (e is TransportException) return SmbException(if (connecting) SmbErrorKind.Network else SmbErrorKind.Disconnected, detail = e.javaClass.simpleName)
        if (e is com.hierynomus.smbj.common.SMBRuntimeException) return SmbException(if (connecting) SmbErrorKind.Protocol else SmbErrorKind.Other, detail = e.javaClass.simpleName)
        if (e is IllegalStateException || e is NullPointerException) return SmbException(if (connecting) SmbErrorKind.Protocol else SmbErrorKind.Disconnected, detail = e.javaClass.simpleName)
        return SmbException(SmbErrorKind.Other, detail = e.javaClass.simpleName)
    }

    /** 通信が壊れた (接続はもう使えない) 種類か */
    fun isConnectionLost(k: SmbErrorKind): Boolean = k == SmbErrorKind.Disconnected || k == SmbErrorKind.Timeout || k == SmbErrorKind.Network
}

/** 利用者が取り消した ([SmbCancel.check] が投げる) */
class SmbCancelled : IOException("smb cancelled")
