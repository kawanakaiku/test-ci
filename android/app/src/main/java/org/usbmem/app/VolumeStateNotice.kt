package org.usbmem.app

import android.content.Context
import org.usbmem.app.ui.Notice
import org.usbmem.app.ui.Severity

/**
 * マウントしたボリュームの状態 ([NtfsVolume.state]: ダーティ / NTFS の未再生ログ / NTFS の休止状態 / メディアエラー) を、
 * 「何が起きていて、何をすればよいか」の通知 (Warning) にする。USB・イメージ・暗号ボリュームの中・入れ子のどの経路も、これを通す (経路ごとに文言を持たない)。
 * 状態が取れていないとき (0) は null を返し、呼び出し側が従来の汎用の通知を出す。docs/dirty-volumes.md
 */
object VolumeStateNotice {
    /** [writeRequested] = 書き込み可で開くことを要求していたか。[writable] = 実際に書き込み可で開いたか。 */
    fun of(ctx: Context, fsName: String, state: Int, writeRequested: Boolean, writable: Boolean): Notice? {
        if (state == 0) return null
        val hib = state and VolState.HIBERNATED != 0
        val log = state and VolState.LOG_UNCLEAN != 0
        val media = state and VolState.MEDIA_ERROR != 0
        val dirty = state and VolState.DIRTY != 0
        val title = when {
            hib -> R.string.notice_vol_hibernated
            log -> R.string.notice_vol_log_unclean
            media -> R.string.notice_vol_media_error
            dirty && writable -> R.string.notice_vol_dirty_writable
            else -> R.string.notice_vol_dirty
        }
        val parts = ArrayList<String>()
        // 原因: 休止状態・未再生ログはダーティフラグの説明を兼ねるので、ダーティだけを別に書くのは、それらが無いときだけ
        if (hib) parts += ctx.getString(R.string.vol_cause_hibernated)
        if (log) parts += ctx.getString(R.string.vol_cause_log_unclean)
        if (dirty && !hib && !log) parts += ctx.getString(when (fsName) {
            "exfat" -> R.string.vol_cause_dirty_exfat
            "fat" -> R.string.vol_cause_dirty_fat
            else -> R.string.vol_cause_dirty_ntfs
        })
        if (media) parts += ctx.getString(R.string.vol_cause_media_error)
        // 結果と、利用者が取れる手段
        if (writable) {
            parts += ctx.getString(R.string.vol_effect_writable_dirty)
        } else {
            parts += ctx.getString(if (writeRequested) R.string.vol_effect_ro_forced else R.string.vol_effect_ro)
            parts += ctx.getString(when {
                hib -> R.string.vol_remedy_hibernated
                log || dirty -> R.string.vol_remedy_dirty
                else -> R.string.vol_remedy_media_error
            })
            if (media && (log || dirty || hib)) parts += ctx.getString(R.string.vol_remedy_media_error)
        }
        return Notice(Severity.Warning, ctx.getString(title), parts.joinToString("\n"))
    }

    fun of(ctx: Context, v: NtfsVolume, writeRequested: Boolean): Notice? = of(ctx, v.fsName, v.state, writeRequested, v.writable)
}
