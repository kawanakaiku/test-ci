package org.usbmem.app

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns

/**
 * SAF の URI から、利用者に見せるファイル名を取る。
 * ダウンロードのピッカーが返す URI の末尾 (`msf:19` / `raw:/storage/emulated/0/Download/x.img`) は内部表記で、そのまま見せると何のファイルか分からない。
 * `OpenableColumns.DISPLAY_NAME` を ContentResolver に問い合わせ、取れなければ (file URI・権限失効・問い合わせの失敗) URI の末尾から作る。IO スレッドで呼ぶ。
 */
object UriNames {
    fun displayName(ctx: Context, uri: Uri, fallback: String = "image"): String {
        if (uri.scheme == "content") {
            try {
                ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                    if (c.moveToFirst()) {
                        val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                        if (i >= 0) c.getString(i)?.takeIf { it.isNotBlank() }?.let { return it }
                    }
                }
            } catch (_: Exception) { /* 取れなければフォールバック */ }
        }
        return fromPath(uri, fallback)
    }

    /** 問い合わせに頼らず、URI の末尾 (`raw:/a/b/x.img` → `x.img`) から作る。 */
    fun fromPath(uri: Uri, fallback: String = "image"): String =
        uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() } ?: fallback
}
