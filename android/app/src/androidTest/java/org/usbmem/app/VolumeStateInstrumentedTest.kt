package org.usbmem.app

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.usbmem.app.ui.Severity
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * ダーティ / 未再生ログ / 休止状態 (高速スタートアップ) / メディアエラーのボリュームの案内 ([VolumeStateNotice]) と、NTFS のその状態のイメージ (tools/gen_ntfs_state_fixture.py) の
 * state() と書き込みの扱い。docs/dirty-volumes.md。イメージは /data/local/tmp/ntfs_state_{clean,dirty,dirty_log,log,hiber}.img (ホストが adb push する。ro-misc の ntfs_read と同じ送り方)。
 */
class VolumeStateInstrumentedTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    // ---- 通知の文言 (原因別)。イメージ不要
    private fun text(n: org.usbmem.app.ui.Notice?) = (n?.text ?: "") + "\n" + (n?.detail ?: "")

    @Test
    fun noticeNamesTheCause() {
        // 何も見つからなければ通知なし (呼び出し側は従来の汎用の通知を出す)
        assertNull(VolumeStateNotice.of(ctx, "ntfs", 0, writeRequested = true, writable = false))

        // 休止状態 (高速スタートアップ): 読み取り専用、原因 (hiberfil.sys / 高速スタートアップ) と、Shift+シャットダウンなどの手順
        val hib = VolumeStateNotice.of(ctx, "ntfs", VolState.HIBERNATED, writeRequested = true, writable = false)
        assertNotNull(hib); assertEquals(Severity.Warning, hib!!.severity)
        assertTrue(text(hib), hib.text.contains("休止状態"))
        assertTrue(text(hib), hib.detail!!.contains("hiberfil.sys") && hib.detail!!.contains("高速スタートアップ") && hib.detail!!.contains("Shift"))
        assertTrue(text(hib), hib.detail!!.contains("書き込みを許可していましたが") && hib.detail!!.contains("読み取り専用で開きました"))
        assertFalse("generic wording must not be used", text(hib).contains("ビルドで無効"))

        // 未再生のログ (ダーティフラグも付く): ログを再生しないこと、内容が古い可能性、Windows で安全に取り外す手順。ダーティだけの段落は重ねない
        val log = VolumeStateNotice.of(ctx, "ntfs", VolState.DIRTY or VolState.LOG_UNCLEAN, writeRequested = true, writable = false)
        assertNotNull(log)
        assertTrue(text(log), log!!.text.contains("書き込みが途中"))
        assertTrue(text(log), log.detail!!.contains("\$LogFile") && log.detail!!.contains("ログを再生しない") && log.detail!!.contains("安全に取り外"))
        assertFalse(text(log), log.detail!!.contains("ダーティフラグが付いています"))
        assertTrue("log is not the hibernation text", !log.detail!!.contains("hiberfil.sys"))

        // ダーティだけ + 読み取り専用で開く (書き込みは要求していない): 「許可していましたが」とは言わない
        val roDirty = VolumeStateNotice.of(ctx, "ntfs", VolState.DIRTY, writeRequested = false, writable = false)
        assertTrue(text(roDirty), roDirty!!.text.contains("正しく取り外されていない") && roDirty.detail!!.contains("NTFS のダーティフラグ") && roDirty.detail!!.contains("読み取り専用で開いています"))
        assertFalse(text(roDirty), roDirty.detail!!.contains("許可していました"))
        assertTrue(text(roDirty), roDirty.detail!!.contains("ログの再生も、ダーティフラグの書き換えもしません"))

        // ダーティだけ (ログは静止) で書き込み可で開いた: その旨 (フラグは閉じるときに下ろす)
        val wDirty = VolumeStateNotice.of(ctx, "ntfs", VolState.DIRTY, writeRequested = true, writable = true)
        assertTrue(text(wDirty), wDirty!!.text.contains("書き込み可で開きました") && wDirty.detail!!.contains("ダーティフラグを下ろします"))

        // exFAT / FAT / メディアエラー
        val ex = VolumeStateNotice.of(ctx, "exfat", VolState.DIRTY, writeRequested = true, writable = false)
        assertTrue(text(ex), ex!!.detail!!.contains("VolumeDirty") && !ex.detail!!.contains("NTFS"))
        val fat = VolumeStateNotice.of(ctx, "fat", VolState.DIRTY, writeRequested = true, writable = false)
        assertTrue(text(fat), fat!!.detail!!.contains("FAT のダーティビット") && !fat.detail!!.contains("NTFS"))
        val media = VolumeStateNotice.of(ctx, "exfat", VolState.MEDIA_ERROR, writeRequested = true, writable = false)
        assertTrue(text(media), media!!.text.contains("メディアエラー") && media.detail!!.contains("chkdsk /r"))
    }

    // ---- NTFS のイメージ (ホストが送ったものだけ。無ければ skip)
    private fun shellCopy(src: String, dst: File): Boolean {
        val pfd = inst.uiAutomation.executeShellCommand("cat $src")
        FileInputStream(pfd.fileDescriptor).use { ins -> dst.outputStream().use { ins.copyTo(it) } }
        pfd.close()
        return dst.length() > 0
    }

    private fun sha256Of(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(f).use { ins -> val buf = ByteArray(1 shl 20); while (true) { val n = ins.read(buf); if (n < 0) break; md.update(buf, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    private fun fetch(name: String): File {
        val img = File(ctx.cacheDir, "ntfs_state_$name.img")
        assumeTrue("fixture not pushed: $name", shellCopy("/data/local/tmp/ntfs_state_$name.img", img))
        return img
    }

    private fun mountRo(img: File): NtfsVolume = NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())

    private fun mountRw(img: File): NtfsVolume {
        try {
            return NtfsVolume.mountReadWrite(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_WRITE).detachFd(), createOnly = false)
        } catch (ex: NativeException) {
            assumeTrue("write support disabled in this build (errno ${ex.code})", ex.code != 30)
            throw ex
        }
    }

    /** 書き込みを止めるべきイメージ: 読み取り専用でも state に原因が出る / rw を要求しても止まる / 1 バイトも変わらない / 通知が原因を言う */
    private fun assertBlocked(name: String, want: Int, titleHas: String, detailHas: String) {
        val img = fetch(name)
        val before = sha256Of(img)
        mountRo(img).use { v ->
            assertEquals("ntfs", v.fsName)
            assertEquals("$name: state (read-only open)", want, v.state)
            assertTrue(v.list("/").any { it.name == "a.txt" })
            val n = VolumeStateNotice.of(ctx, v, writeRequested = false)
            assertTrue("$name: ${text(n)}", n != null && n.text.contains(titleHas) && n.detail!!.contains(detailHas) && n.detail!!.contains("読み取り専用で開いています"))
        }
        mountRw(img).use { v ->
            assertEquals("$name: state (rw requested)", want, v.state)
            assertFalse("$name must not be writable", v.writable)
            val n = VolumeStateNotice.of(ctx, v, writeRequested = true)
            assertTrue("$name: ${text(n)}", n != null && n.text.contains(titleHas) && n.detail!!.contains(detailHas) && n.detail!!.contains("書き込みを許可していましたが"))
            try { v.create("/", "x.txt", false); throw AssertionError("create must fail") } catch (ex: NativeException) { assertEquals(30, ex.code) }
        }
        assertEquals("$name: the image must stay byte-identical (no log emptying, no flag change)", before, sha256Of(img))
        img.delete()
    }

    @Test
    fun hibernatedVolumeStaysReadOnly() = assertBlocked("hiber", VolState.HIBERNATED, "休止状態", "hiberfil.sys")

    @Test
    fun unreplayedLogVolumeStaysReadOnly() = assertBlocked("dirty_log", VolState.DIRTY or VolState.LOG_UNCLEAN, "書き込みが途中", "未再生のログ")

    @Test
    fun unreplayedLogWithoutDirtyFlagStaysReadOnly() = assertBlocked("log", VolState.LOG_UNCLEAN, "書き込みが途中", "未再生のログ")

    @Test
    fun cleanVolumeHasNoState() {
        val img = fetch("clean")
        mountRo(img).use { v -> assertEquals(0, v.state); assertNull(VolumeStateNotice.of(ctx, v, writeRequested = false)) }
        mountRw(img).use { v -> assertEquals(0, v.state); assertTrue(v.writable); assertNull(VolumeStateNotice.of(ctx, v, writeRequested = true)) }
        img.delete()
    }

    @Test
    fun dirtyFlagOnlyKeepsTheExistingPolicy() {
        // ダーティフラグだけ (ログは静止) は従来どおり書き込み可。クリーンなアンマウントでフラグが下りる (上流の方針)
        val img = fetch("dirty")
        mountRo(img).use { v ->
            assertEquals(VolState.DIRTY, v.state)
            val n = VolumeStateNotice.of(ctx, v, writeRequested = false)
            assertTrue(text(n), n != null && n.detail!!.contains("NTFS のダーティフラグ") && n.detail!!.contains("読み取り専用で開いています"))
        }
        mountRw(img).use { v ->
            assertEquals(VolState.DIRTY, v.state)
            assertTrue("dirty flag with a quiet log is writable", v.writable)
            val n = VolumeStateNotice.of(ctx, v, writeRequested = true)
            assertTrue(text(n), n != null && n.text.contains("書き込み可で開きました"))
            v.create("/", "new.txt", false)
        }
        mountRo(img).use { v -> assertEquals("the dirty flag is lowered by the clean unmount", 0, v.state) }
        img.delete()
    }
}
