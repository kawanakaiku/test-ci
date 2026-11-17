package org.usbmem.app

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.test.espresso.intent.Intents
import androidx.test.espresso.intent.matcher.IntentMatchers.hasAction
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.usbmem.app.ui.AppModel
import org.usbmem.app.ui.UsbDeviceState
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

/**
 * ダーティフラグだけの NTFS に書き込み可を要求したときの確認ダイアログ (「読み取り専用で開く」/「書き込み可で開く」/ キャンセル) を、実画面の操作で通す試験。docs/dirty-volumes.md
 * 確認の前は何も書かない (イメージ不変・何も公開しない)。イメージは VolumeStateInstrumentedTest と同じ /data/local/tmp/ntfs_state_dirty.img (ホストが adb push する)。
 *  - イメージ画面 (「書き込みを許可」+ イメージを共有): 回転 (Activity の再作成) でダイアログが出し直されることも。
 *  - USB 画面: 実機の USB が無いので、偽のセッション (マウントは実イメージ) を UsbSessions.testOverride で差し込み、MainActivity.publishPartition を通す。
 */
class DirtyConfirmUiTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    private fun s(id: Int) = ctx.getString(id)
    private fun tag(t: String) = rule.onNodeWithTag(t)
    private fun clickTag(t: String) { val n = rule.onAllNodesWithTag(t)[0]; try { n.performScrollTo() } catch (_: AssertionError) { }; n.performClick() }
    private fun waitTag(t: String, ms: Long = 60_000) { rule.waitUntil(ms) { rule.onAllNodesWithTag(t).fetchSemanticsNodes().isNotEmpty() } }
    private fun waitText(t: String, ms: Long = 60_000) { rule.waitUntil(ms) { rule.onAllNodesWithText(t, substring = true).fetchSemanticsNodes().isNotEmpty() } }
    private fun waitGone(t: String, ms: Long = 60_000) { rule.waitUntil(ms) { rule.onAllNodesWithTag(t).fetchSemanticsNodes().isEmpty() } }

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

    private lateinit var img: File
    private lateinit var before: String

    @Before fun setUp() {
        assumeTrue("write support disabled in this build", BuildConfig.WRITE_SUPPORTED)
        img = File(ctx.cacheDir, "ntfs_state_dirty.img")
        img.delete()
        assumeTrue("fixture not pushed: ntfs_state_dirty.img", shellCopy("/data/local/tmp/ntfs_state_dirty.img", img))
        before = sha256Of(img)
        Intents.init()
    }

    @After fun tearDown() {
        runCatching { Intents.release() }
        UsbSessions.testOverride?.closeAll()
        UsbSessions.testOverride = null
        rule.runOnUiThread { AppModel.shared.usbDevices = emptyList() }
        for (e in VolumeRegistry.all(ctx).toList()) runCatching { VolumeRegistry.remove(ctx, e.id) }
        if (::img.isInitialized) img.delete()
    }

    private fun entries() = VolumeRegistry.all(ctx).filter { it.label == img.name || it.deviceName != null }
    private fun mountRo(): NtfsVolume = NtfsVolume.mountReadOnly(ParcelFileDescriptor.open(img, ParcelFileDescriptor.MODE_READ_ONLY).detachFd())

    // ================================================================ イメージ画面

    private fun stubPick() {
        Intents.release(); Intents.init()
        Intents.intending(hasAction(Intent.ACTION_OPEN_DOCUMENT)).respondWith(Instrumentation.ActivityResult(Activity.RESULT_OK, Intent().setData(Uri.fromFile(img))))
    }

    /** イメージ画面で「書き込みを許可」にして、イメージを共有する (ファイル選択は差し替え)。確認のダイアログが出るまで待つ。 */
    private fun shareImageWritable() {
        stubPick()
        rule.onNodeWithText(s(R.string.tab_image)).performClick()
        clickTag("image_write_switch")
        clickTag("image_share_button")
        waitTag("dirty_dialog")
    }

    private fun assertAsked() {
        assertTrue("nothing may be published before the answer", entries().isEmpty())
        assertEquals("the image must stay byte-identical while asking", before, sha256Of(img))
        rule.onNodeWithText(s(R.string.dirty_dialog_text), substring = true).assertExists()
        tag("dirty_open_ro").assertExists(); tag("dirty_open_rw").assertExists(); tag("dirty_cancel").assertExists()
    }

    @Test fun image_asksBeforeWriting_thenReadOnly() {
        shareImageWritable()
        assertAsked()
        clickTag("dirty_open_ro")
        rule.waitUntil(60_000) { entries().isNotEmpty() }
        val e = entries().single()
        assertFalse("read-only was chosen", e.volume.writable)
        assertEquals(VolState.DIRTY, e.volume.state)
        waitGone("dirty_dialog")
        assertEquals("the read-only open must not change the image", before, sha256Of(img))
        waitText(s(R.string.notice_vol_dirty))   // 通知は従来どおり (読み取り専用で開いています)
    }

    @Test fun image_asksBeforeWriting_thenWrite() {
        shareImageWritable()
        assertAsked()
        clickTag("dirty_open_rw")
        rule.waitUntil(60_000) { entries().isNotEmpty() }
        val e = entries().single()
        assertTrue("write was chosen", e.volume.writable)
        e.volume.create("/", "confirmed.txt", false)
        waitText(s(R.string.notice_vol_dirty_writable))
        VolumeRegistry.remove(ctx, e.id)   // クリーンにアンマウント → ダーティフラグが下りる
        mountRo().use { v ->
            assertEquals("the dirty flag is lowered by the clean unmount", 0, v.state)
            assertTrue(v.list("/").any { it.name == "confirmed.txt" })
        }
    }

    @Test fun image_asksBeforeWriting_thenCancel() {
        shareImageWritable()
        assertAsked()
        clickTag("dirty_cancel")
        waitGone("dirty_dialog")
        waitText(s(R.string.dirty_cancelled))
        assertTrue("cancel opens nothing", entries().isEmpty())
        assertEquals(before, sha256Of(img))
    }

    @Test fun image_dialogSurvivesRecreate() {
        shareImageWritable()
        assertAsked()
        rule.activityRule.scenario.recreate()   // 回転など: ダイアログは出し直される (保留しているものは何も無い。開き直すだけ)
        waitTag("dirty_dialog")
        assertAsked()
        clickTag("dirty_open_ro")
        rule.waitUntil(60_000) { entries().isNotEmpty() }
        assertFalse(entries().single().volume.writable)
        assertEquals(before, sha256Of(img))
    }

    // ================================================================ USB 画面 (偽のセッション。マウントは実イメージ)

    private class FakeSession(override val deviceName: String, override val writable: Boolean, private val image: File) : UsbSession {
        @Volatile var closed = false
        override val vendorId = 0x0781; override val productId = 0x5581; override val tuningIndex = -1
        override fun partitionTable() = PartitionTable("mbr", 512, image.length(), "", false, false, emptyList(), emptyList())
        override fun opStats() = ""
        override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean, allowDirty: Boolean): NtfsVolume {
            check(!closed)
            val fd = ParcelFileDescriptor.open(image, if (writable) ParcelFileDescriptor.MODE_READ_WRITE else ParcelFileDescriptor.MODE_READ_ONLY).detachFd()
            return if (writable) NtfsVolume.mountReadWrite(fd, createOnly, allowDirty) else NtfsVolume.mountReadOnly(fd, allowNoload)
        }
        override fun startStop(start: Boolean) {}
        override fun isSpunDown() = false
        override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) {}
        override fun tune(chunkKiB: Int, maxTransferKiB: Int) {}
        override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray) = 1L
        override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int) = 1L
        override fun <T> withHandle(block: (Long) -> T): T = block(0)
        override fun close() { closed = true }
    }

    private val usbName = "/dev/bus/usb/001/042"

    /** 書き込み可 (すべて許可) で開いた偽の USB を、画面の状態にも載せる。 */
    private fun openFakeUsb(): UsbDeviceState {
        val mgr = UsbSessionManager(
            object : UsbSessionFactory {
                override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession = FakeSession(device.deviceName, writable, img)
            },
            onDeviceClosing = { name -> try { VolumeRegistry.removeByDevice(name) } finally { VolumeRegistry.notifyRoots(ctx) } },
        )
        UsbSessions.testOverride = mgr
        assertTrue(mgr.setWriteMode(usbName, WriteMode.Full))
        mgr.open(UsbDeviceInfo(usbName, "Fake Stick", 0x0781, 0x5581))
        val st = UsbDeviceState(usbName).also { it.title = "Fake Stick"; it.open = true; it.writable = true; it.writeMode = WriteMode.Full }
        rule.runOnUiThread { AppModel.shared.usbDevices = listOf(st) }
        return st
    }

    private fun usbPublish(st: UsbDeviceState) {
        rule.runOnUiThread { rule.activity.publishPartition(usbName, 0, img.length(), "dirty-usb") }
        waitTag("dirty_dialog")
        assertTrue("nothing may be published before the answer", st.published.isEmpty() && VolumeRegistry.all(ctx).none { it.deviceName != null })
        assertNull(st.noloadPrompt)
        assertNotNull(st.dirtyPrompt)
        assertEquals("the image must stay byte-identical while asking", before, sha256Of(img))
    }

    @Test fun usb_asksBeforeWriting_thenReadOnly() {
        val st = openFakeUsb()
        usbPublish(st)
        clickTag("dirty_open_ro")
        rule.waitUntil(60_000) { st.published.isNotEmpty() }
        waitGone("dirty_dialog")
        assertFalse("read-only was chosen", st.published.single().writable)
        val e = VolumeRegistry.all(ctx).single { it.deviceName != null }
        assertFalse(e.volume.writable)
        assertEquals(before, sha256Of(img))
        rule.waitUntil(60_000) { st.notice?.detail?.contains("読み取り専用で開いています") == true }
    }

    @Test fun usb_asksBeforeWriting_thenWrite() {
        val st = openFakeUsb()
        usbPublish(st)
        clickTag("dirty_open_rw")
        rule.waitUntil(60_000) { st.published.isNotEmpty() }
        assertTrue("write was chosen", st.published.single().writable)
        val pub = st.published.single()
        val e = VolumeRegistry.all(ctx).single { it.deviceName != null }
        assertTrue(e.volume.writable)
        e.volume.create("/", "usb_confirmed.txt", false)
        rule.runOnUiThread { rule.activity.unpublish(usbName, pub.volumeKey) }   // クリーンにアンマウント → フラグが下りる
        rule.waitUntil(60_000) { st.published.isEmpty() && VolumeRegistry.all(ctx).none { it.deviceName != null } }
        mountRo().use { v ->
            assertEquals("the dirty flag is lowered by the clean unmount", 0, v.state)
            assertTrue(v.list("/").any { it.name == "usb_confirmed.txt" })
        }
    }

    @Test fun usb_asksBeforeWriting_thenCancel() {
        val st = openFakeUsb()
        usbPublish(st)
        clickTag("dirty_cancel")
        waitGone("dirty_dialog")
        assertNull(st.dirtyPrompt)
        assertTrue("cancel opens nothing", st.published.isEmpty() && VolumeRegistry.all(ctx).none { it.deviceName != null })
        assertEquals(before, sha256Of(img))
    }

    @Test fun usb_detachWhileAskingLeaksNothing() {
        val st = openFakeUsb()
        usbPublish(st)
        // 確認を待っている間に USB が閉じられた (抜かれた): ダイアログは消え、何も残らない (保留しているものは何も無い)
        UsbSessions.testOverride!!.close(usbName)
        rule.runOnUiThread { st.resetAfterClose() }   // 本番は refreshUsb / 抜去の通知がこれを呼ぶ
        waitGone("dirty_dialog")
        assertNull(st.dirtyPrompt)
        assertTrue(VolumeRegistry.all(ctx).none { it.deviceName != null })
        assertEquals(before, sha256Of(img))
    }

    @Test fun usb_readOnlySessionNeverAsks() {
        // 読み取り専用で開いた USB は、確認なしで読み取り専用 (従来どおり。ダイアログを出さない)
        val mgr = UsbSessionManager(
            object : UsbSessionFactory { override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession = FakeSession(device.deviceName, writable, img) },
            onDeviceClosing = { name -> VolumeRegistry.removeByDevice(name) },
        )
        UsbSessions.testOverride = mgr
        mgr.open(UsbDeviceInfo(usbName, "Fake Stick", 0x0781, 0x5581))
        val st = UsbDeviceState(usbName).also { it.title = "Fake Stick"; it.open = true; it.writable = false }
        rule.runOnUiThread { AppModel.shared.usbDevices = listOf(st) }
        rule.runOnUiThread { rule.activity.publishPartition(usbName, 0, img.length(), "ro-usb") }
        rule.waitUntil(60_000) { st.published.isNotEmpty() }
        assertNull(st.dirtyPrompt)
        assertEquals(0, rule.onAllNodesWithTag("dirty_dialog").fetchSemanticsNodes().size)
        assertFalse(st.published.single().writable)
        assertEquals(before, sha256Of(img))
    }
}
