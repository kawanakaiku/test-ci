// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import android.content.Context
import org.usbmem.app.VolumeRegistry

/**
 * マウント中の SMB 共有の登録簿 (プロセス内。USB の [VolumeRegistry] とは別)。SAF のプロバイダ ([SmbDocuments]) が参照する。
 * **永続化しない**: パスワードを持たないので、アプリを終了したら全部消え、次はマウントし直す (USB のボリュームと同じ)。
 * ドキュメント ID = "<[Mount.id]>:<パス>" ("smb-" で始まるので、USB のボリューム ID と区別できる)。
 */
object SmbMounts {
    const val ID_PREFIX = "smb-"

    /** [writable] = 書き込みを許してマウントした (利用者が選ぶ。サーバーが許さなければ失敗する) */
    class Mount(val conn: SmbConnection, val fs: RemoteFs, val writable: Boolean) {
        val id: String = idOf(conn.id)
        /** SAF のルートの名前と画面の表示 ("<接続名> (SMB)") */
        val label: String get() = conn.name + " (SMB)"
    }

    fun idOf(connId: String): String = ID_PREFIX + connId

    private val mounts = LinkedHashMap<String, Mount>()
    /** 切れて外した共有 (接続の id → 理由)。画面が一度だけ知らせる */
    private val lostReasons = LinkedHashMap<String, SmbErrorKind>()

    @Synchronized fun all(): List<Mount> = mounts.values.toList()
    @Synchronized fun get(id: String): Mount? = mounts[id]
    @Synchronized fun byConnection(connId: String): Mount? = mounts[idOf(connId)]

    /** 登録する。同じ接続の古いマウントがあれば閉じる (閉じる処理はロックの外)。 */
    fun add(ctx: Context, m: Mount) {
        val old = synchronized(this) { val o = mounts.put(m.id, m); lostReasons.remove(m.conn.id); o }
        if (old != null && old !== m) closeQuietly(old.fs)
        notifyRoots(ctx)
    }

    /** マウントを外して接続を閉じる。無ければ false。閉じる処理 (ログオフ) は遅いことがあるので、呼び出し元は UI スレッドでないこと。 */
    fun remove(ctx: Context, id: String): Boolean {
        val m = synchronized(this) { mounts.remove(id) } ?: return false
        closeQuietly(m.fs)
        notifyRoots(ctx)
        return true
    }

    fun removeByConnection(ctx: Context, connId: String): Boolean = remove(ctx, idOf(connId))

    /** 全部外す (アプリの終了・テストの後始末) */
    fun removeAll(ctx: Context) {
        val l = synchronized(this) { val v = mounts.values.toList(); mounts.clear(); v }
        l.forEach { closeQuietly(it.fs) }
        if (l.isNotEmpty()) notifyRoots(ctx)
    }

    /**
     * 切れた (isAlive が false の) マウントを外す。外したものの接続の id と理由を [lostReasons] に残す。SAF のルート一覧を作るたび・画面の更新のたびに呼ぶ。
     * 戻り値 = 外した数。
     */
    fun reapDead(ctx: Context): Int {
        val dead = synchronized(this) {
            val d = mounts.values.filter { !it.fs.isAlive }
            d.forEach { mounts.remove(it.id); lostReasons[it.conn.id] = SmbErrorKind.Disconnected }
            d
        }
        dead.forEach { closeQuietly(it.fs) }
        if (dead.isNotEmpty()) notifyRoots(ctx)
        return dead.size
    }

    /** 画面が「切断されました」を一度だけ出すために取り出す (取り出したら忘れる) */
    @Synchronized fun takeLost(connId: String): SmbErrorKind? = lostReasons.remove(connId)

    fun notifyRoots(ctx: Context) {
        try { VolumeRegistry.notifyRoots(ctx) } catch (_: Exception) { }
    }

    private fun closeQuietly(fs: RemoteFs) { try { fs.close() } catch (_: Exception) { } }
}
