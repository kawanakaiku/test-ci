package org.usbmem.app

import android.content.Context

/**
 * USB の読み取り転送サイズ (USB 1 回の転送要求 KiB / 1 SCSI READ コマンド KiB) の、端末 + USB 機器ごとの保存。
 *
 * 通る最大サイズは端末ごとに違う (F-51A は 64K/256K で高速、J9260 は 64K/256K で EPROTO、Xperia は 128K/512K まで可)。デバイスの申告値や USB の記述子からは決まらないので、
 * 「最適化」ボタンで小さい設定から順に実際に読んで確かめ、**ハッシュが一致して通った中で最速**のものを保存し、次回の open から使う。書き込みの転送サイズは変えない。
 *
 * 試す前に「試行中」を保存し、結果が出たら消す。試行中にアプリが落ちた・USB が切れた場合、次回の最適化はその設定以上を「失敗」とみなして試さない (同じ所で同じ失敗を繰り返さない)。
 * 保存は SharedPreferences (この端末のアプリ内のみ)。キーは USB のベンダ ID:プロダクト ID。
 */
object TransferTuning {
    /** 小さい順の候補 (chunk KiB, cmd KiB)。chunk は 16 の倍数。256K 以上の転送要求は F-51A の RAID で io-error だったので候補にしない。 */
    val LADDER = listOf(16 to 64, 64 to 128, 64 to 256, 128 to 256, 128 to 512)
    private const val PREFS = "usbmem_tuning"

    private fun key(vid: Int, pid: Int) = "%04x:%04x".format(vid, pid)

    /** 保存済みの設定 (無ければ null)。範囲外・不正な値は無視する。 */
    fun load(ctx: Context, vid: Int, pid: Int): Pair<Int, Int>? {
        val i = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(key(vid, pid) + ":best", -1)
        return LADDER.getOrNull(i)
    }

    /** これ以上の添字は失敗済み (試さない)。未記録は LADDER.size (= 全部可)。 */
    fun failedFrom(ctx: Context, vid: Int, pid: Int): Int {
        val p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val pending = p.getInt(key(vid, pid) + ":trying", -1)
        val failed = p.getInt(key(vid, pid) + ":failed", LADDER.size)
        // 試行中のまま残っている = その試行で落ちた (または USB が切れた)
        return if (pending in LADDER.indices) minOf(failed, pending) else failed
    }

    fun markTrying(ctx: Context, vid: Int, pid: Int, index: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt(key(vid, pid) + ":trying", index).commit()
    }

    fun markOk(ctx: Context, vid: Int, pid: Int, best: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt(key(vid, pid) + ":best", best).remove(key(vid, pid) + ":trying").commit()
    }

    fun markFailed(ctx: Context, vid: Int, pid: Int, index: Int) {
        val p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val old = p.getInt(key(vid, pid) + ":failed", LADDER.size)
        p.edit().putInt(key(vid, pid) + ":failed", minOf(old, index)).remove(key(vid, pid) + ":trying").commit()
    }

    /** 試行中の印だけを消す (結果を保存しないとき)。 */
    fun clearTrying(ctx: Context, vid: Int, pid: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(key(vid, pid) + ":trying").commit()
    }

    /** 保存を捨てて既定 (16K/64K) に戻す。保存した設定でマウントが失敗したときなど。 */
    fun reset(ctx: Context, vid: Int, pid: Int) {
        val k = key(vid, pid)
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove("$k:best").remove("$k:trying").remove("$k:failed").commit()
    }
}
