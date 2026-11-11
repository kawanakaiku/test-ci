package org.usbmem.app

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.security.MessageDigest

/** マウント済み NTFS ボリュームの登録簿。プロセス内で共有し、SAF の URI は端末内の private な設定に保存して再マウントする。 */
object VolumeRegistry {
    class Entry(val id: String, val label: String, val volume: NtfsVolume)

    private val volumes = LinkedHashMap<String, Entry>()
    private var restored = false
    private const val PREFS = "volumes"

    private fun idOf(key: String): String =
        MessageDigest.getInstance("SHA-256").digest(key.toByteArray()).take(6).joinToString("") { "%02x".format(it) }

    @Synchronized
    fun all(ctx: Context): List<Entry> { restore(ctx); return volumes.values.toList() }

    @Synchronized
    fun byKey(key: String): Entry? = volumes[idOf(key)]

    @Synchronized
    fun get(ctx: Context, id: String): Entry? { restore(ctx); return volumes[id] }

    /** SAF で選ばれたイメージを読み取り専用でマウントして登録する。 */
    @Synchronized
    fun addUri(ctx: Context, uri: Uri, writable: Boolean = false): Entry {
        restore(ctx)
        val e = mountUri(ctx, uri, writable)
        // 再起動後の復元は常に読み取り専用 (書き込みは起動のたびに明示的に許可させる)
        prefs(ctx).edit().putStringSet("uris", (savedUris(ctx) + uri.toString()).toSet()).apply()
        return e
    }

    /** テスト・内部用: 既にある fd(所有権を渡す)を登録する。永続化しない。 */
    @Synchronized
    fun addFd(label: String, key: String, detachedFd: Int, writable: Boolean = false, createOnly: Boolean = false): Entry {
        val id = idOf(key)
        volumes.remove(id)?.volume?.close()
        val e = Entry(id, label, if (writable) NtfsVolume.mountReadWrite(detachedFd, createOnly) else NtfsVolume.mountReadOnly(detachedFd))
        volumes[id] = e
        return e
    }

    /** マウント済みの NTFS ボリュームを登録する (USB など。永続化しない: 再接続時に再度登録される)。所有権は登録簿に移る。 */
    @Synchronized
    fun addVolume(label: String, key: String, volume: NtfsVolume): Entry {
        val id = idOf(key)
        volumes.remove(id)?.volume?.close()
        return Entry(id, label, volume).also { volumes[id] = it }
    }

    @Synchronized
    fun removeByKey(key: String) { volumes.remove(idOf(key))?.volume?.close() }

    @Synchronized
    fun remove(ctx: Context, id: String) {
        volumes.remove(id)?.volume?.close()
    }

    private fun mountUri(ctx: Context, uri: Uri, writable: Boolean = false): Entry {
        val pfd: ParcelFileDescriptor = ctx.contentResolver.openFileDescriptor(uri, if (writable) "rw" else "r")
            ?: throw java.io.FileNotFoundException(uri.toString())
        val label = uri.lastPathSegment?.substringAfterLast('/') ?: "NTFS"
        return addFd(label, uri.toString(), pfd.detachFd(), writable)
    }

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private fun savedUris(ctx: Context): Set<String> = prefs(ctx).getStringSet("uris", emptySet()) ?: emptySet()

    private fun restore(ctx: Context) {
        if (restored) return
        restored = true
        for (u in savedUris(ctx)) {
            try { mountUri(ctx, Uri.parse(u)) } catch (_: Exception) { /* 権限失効・削除済みは黙って捨てる */ }
        }
    }
}
