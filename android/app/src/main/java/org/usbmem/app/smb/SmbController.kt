// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import android.content.Context
import org.usbmem.app.R
import org.usbmem.app.RecallResult
import org.usbmem.app.RememberResult
import org.usbmem.app.SecretVault
import org.usbmem.app.VaultProvider
import org.usbmem.app.VaultSupport
import java.nio.ByteBuffer
import java.nio.CharBuffer

/** パスワードのバイト列 / 文字配列の変換と消去。Kotlin の String は消せないので、画面を出たあとに渡す値は必ず配列にする。 */
object SmbSecrets {
    fun wipe(a: ByteArray?) { if (a != null) java.util.Arrays.fill(a, 0.toByte()) }
    fun wipe(a: CharArray?) { if (a != null) java.util.Arrays.fill(a, '\u0000') }

    /** UTF-8 のバイト列にする (途中に出る一時バッファもゼロで埋める) */
    fun toBytes(chars: CharArray): ByteArray {
        val bb: ByteBuffer = Charsets.UTF_8.encode(CharBuffer.wrap(chars))
        try {
            val out = ByteArray(bb.remaining())
            bb.get(out)
            return out
        } finally {
            if (bb.hasArray()) java.util.Arrays.fill(bb.array(), 0.toByte())
        }
    }

    fun toChars(bytes: ByteArray): CharArray {
        val cb: CharBuffer = Charsets.UTF_8.decode(ByteBuffer.wrap(bytes))
        try {
            val out = CharArray(cb.remaining())
            cb.get(out)
            return out
        } finally {
            if (cb.hasArray()) java.util.Arrays.fill(cb.array(), '\u0000')
        }
    }
}

/** 保存した接続の一覧 (アプリ専用の SharedPreferences。**パスワードは入らない**) */
class SmbStore(ctx: Context) {
    private val prefs = ctx.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    @Synchronized fun all(): List<SmbConnection> = SmbConnection.listFromJson(prefs.getString(KEY, null))

    @Synchronized fun get(id: String): SmbConnection? = all().firstOrNull { it.id == id }

    /** 追加または同じ id を置き換える */
    @Synchronized fun save(c: SmbConnection) {
        val l = all().toMutableList()
        val i = l.indexOfFirst { it.id == c.id }
        if (i >= 0) l[i] = c else l.add(c)
        prefs.edit().putString(KEY, SmbConnection.listToJson(l)).commit()
    }

    @Synchronized fun delete(id: String) {
        prefs.edit().putString(KEY, SmbConnection.listToJson(all().filter { it.id != id })).commit()
    }

    private companion object {
        const val PREFS = "usbmem_smb"
        const val KEY = "connections"
    }
}

/** 取り出したパスワード (保存してあった分)。[Password.chars] は呼び出し側がゼロで埋める */
sealed class SmbRecalled {
    class Password(val chars: CharArray) : SmbRecalled()
    object Cancelled : SmbRecalled()
    /** 鍵が使えなくなっていた (生体登録の変更など)。保存は破棄済み。入力し直す */
    object Invalidated : SmbRecalled()
    object NotSaved : SmbRecalled()
    class Failed(val message: String) : SmbRecalled()
}

data class SmbTestResult(val dialect: String, val encrypted: Boolean, val signed: Boolean, val serverAllowsWrite: Boolean, val rootEntries: Int)

/**
 * SMB 接続の操作の入口 (画面と計装テストが使う)。ネットワークと Keystore (生体認証の待ち) を使うので、**どれもメインスレッドから呼ばない**。
 *
 * パスワードの扱い (docs/smb.md 4 章): 保存は既定でしない。保存を選んだときだけ、[VaultProvider] (暗号ボリュームのパスワードの記憶と同じ仕組み:
 * Android Keystore の AES-256-GCM 鍵 + 使うたびに生体認証 / 画面ロック) で暗号化して置く。受け取った配列は、接続に使ったあと**必ずゼロで埋める**。ログ・例外のメッセージには入れない。
 */
object SmbController {
    /** パスワードの記憶の鍵 (接続ごと。ホスト名などを含まない) */
    fun vaultId(c: SmbConnection): String = "smb:" + c.id

    private fun vault(ctx: Context): SecretVault = VaultProvider.get(ctx)

    /** パスワードの記憶が、この端末で使えるか (使えないなら理由) */
    fun support(ctx: Context): VaultSupport = vault(ctx).supportState()

    fun hasSavedPassword(ctx: Context, c: SmbConnection): Boolean = try { vault(ctx).has(vaultId(c)) } catch (_: Exception) { false }

    /**
     * 接続・認証・共有の一覧まで試す (何もマウントしない)。[password] は使ったあとゼロで埋める。失敗は [SmbException]。
     */
    fun test(conn: SmbConnection, password: CharArray, cancel: SmbCancel? = null, timeoutSec: Int = SmbRemoteFs.DEFAULT_TIMEOUT_SEC): SmbTestResult {
        val fs = SmbRemoteFs.connect(conn, password, cancel, timeoutSec)
        try {
            val n = fs.list("/").size
            return SmbTestResult(fs.dialect, fs.encrypted, fs.signed, fs.serverAllowsWrite, n)
        } catch (e: Throwable) {
            throw SmbErrors.map(e)
        } finally {
            fs.close()
        }
    }

    /**
     * マウントする (SAF のルートに「<接続名> (SMB)」が出る)。[writable] はサーバーが書き込みを許すときだけ有効になる。[password] は使ったあとゼロで埋める。
     * 同じ接続が既にマウントされていれば、置き換える。
     */
    fun mount(ctx: Context, conn: SmbConnection, password: CharArray, writable: Boolean, cancel: SmbCancel? = null, timeoutSec: Int = SmbRemoteFs.DEFAULT_TIMEOUT_SEC): SmbMounts.Mount {
        val fs = SmbRemoteFs.connect(conn, password, cancel, timeoutSec)
        try {
            fs.list("/")   // 共有のルートを開けること (権限・名前)
        } catch (e: Throwable) {
            try { fs.close() } catch (_: Exception) { }
            throw SmbErrors.map(e)
        }
        val m = SmbMounts.Mount(conn, fs, writable && fs.serverAllowsWrite)
        SmbMounts.add(ctx.applicationContext, m)
        return m
    }

    fun unmount(ctx: Context, conn: SmbConnection): Boolean = SmbMounts.removeByConnection(ctx.applicationContext, conn.id)

    /** パスワードを Keystore で暗号化して保存する (認証の画面が出る)。[password] は消さない (呼び出し側が消す)。 */
    fun rememberPassword(ctx: Context, conn: SmbConnection, password: CharArray): RememberResult {
        if (password.isEmpty()) return RememberResult.Failed("empty")
        val bytes = SmbSecrets.toBytes(password)
        try {
            return vault(ctx).remember(vaultId(conn), bytes, ctx.getString(R.string.smb_vault_save_title), conn.name)
        } finally { SmbSecrets.wipe(bytes) }
    }

    /** 保存したパスワードを取り出す (認証の画面が出る)。 */
    fun recallPassword(ctx: Context, conn: SmbConnection): SmbRecalled {
        return when (val r = vault(ctx).recall(vaultId(conn), ctx.getString(R.string.smb_vault_unlock_title), conn.name)) {
            is RecallResult.Secret -> try { SmbRecalled.Password(SmbSecrets.toChars(r.bytes)) } finally { SmbSecrets.wipe(r.bytes) }
            RecallResult.Cancelled -> SmbRecalled.Cancelled
            RecallResult.Invalidated -> SmbRecalled.Invalidated
            RecallResult.NotRemembered -> SmbRecalled.NotSaved
            is RecallResult.Failed -> SmbRecalled.Failed(r.message)
        }
    }

    /** 保存したパスワードを消す (暗号文と鍵)。 */
    fun forgetPassword(ctx: Context, conn: SmbConnection) { try { vault(ctx).forget(vaultId(conn)) } catch (_: Exception) { } }

    /** 接続の設定を消す: マウントを外し、保存したパスワードも消し、一覧から外す。 */
    fun delete(ctx: Context, conn: SmbConnection) {
        unmount(ctx, conn)
        forgetPassword(ctx, conn)
        SmbStore(ctx).delete(conn.id)
    }
}
