package org.usbmem.app

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** 転送サイズの保存 (端末 + USB 機器ごと)。失敗した設定は二度と試さない / 試行中に落ちたら失敗扱い / 機器ごとに独立。 */
class TransferTuningTest {
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private val n = TransferTuning.LADDER.size

    @Test
    fun saveLoadAndFailureBookkeeping() {
        val vid = 0x1412; val pid = 0x9999   // テスト専用 (実機の RAID とは別キー)
        TransferTuning.reset(ctx, vid, pid)
        assertNull(TransferTuning.load(ctx, vid, pid))
        assertEquals(n, TransferTuning.failedFrom(ctx, vid, pid))

        // 通ったら保存 → 読める。試行中の印は消える
        TransferTuning.markTrying(ctx, vid, pid, 2)
        TransferTuning.markOk(ctx, vid, pid, 2)
        assertEquals(TransferTuning.LADDER[2], TransferTuning.load(ctx, vid, pid))
        assertEquals(n, TransferTuning.failedFrom(ctx, vid, pid))

        // 試行中のまま (アプリが落ちた・USB が切れた) = その設定以上は失敗扱い
        TransferTuning.markTrying(ctx, vid, pid, 4)
        assertEquals(4, TransferTuning.failedFrom(ctx, vid, pid))
        TransferTuning.markFailed(ctx, vid, pid, 4)
        assertEquals(4, TransferTuning.failedFrom(ctx, vid, pid))
        // 失敗の添字は小さい方へだけ更新される (一度失敗した所を「可」に戻さない)
        TransferTuning.markFailed(ctx, vid, pid, 5.coerceAtMost(n))
        assertEquals(4, TransferTuning.failedFrom(ctx, vid, pid))
        TransferTuning.markTrying(ctx, vid, pid, 3); TransferTuning.markFailed(ctx, vid, pid, 3)
        assertEquals(3, TransferTuning.failedFrom(ctx, vid, pid))
        assertEquals(TransferTuning.LADDER[2], TransferTuning.load(ctx, vid, pid))   // 保存済みの最速は失敗の記録では消えない

        // 機器ごとに独立
        assertNull(TransferTuning.load(ctx, vid, pid + 1))
        assertEquals(n, TransferTuning.failedFrom(ctx, vid, pid + 1))

        // 保存を捨てる (保存した設定でマウントが失敗したとき) → 既定へ
        TransferTuning.reset(ctx, vid, pid)
        assertNull(TransferTuning.load(ctx, vid, pid))
        assertEquals(n, TransferTuning.failedFrom(ctx, vid, pid))
    }

    @Test
    fun ladderIsAscendingAndChunksAre16KMultiples() {
        val l = TransferTuning.LADDER
        assertEquals(16 to 64, l[0])   // 先頭は既定 (基準のハッシュを取る設定)
        for (i in 1 until l.size) assertTrue(l[i].first >= l[i - 1].first && l[i].second >= l[i - 1].second && l[i] != l[i - 1])
        for ((c, m) in l) { assertEquals(0, c % 16); assertTrue(m >= c) }
        assertTrue(l.all { it.first <= 128 })   // 256K 以上の転送要求は F-51A の RAID で io-error だった
    }
}
