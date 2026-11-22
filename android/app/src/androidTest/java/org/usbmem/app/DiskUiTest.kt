// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app

import android.app.Activity
import android.app.Instrumentation
import android.app.UiAutomation
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.ParcelFileDescriptor
import androidx.compose.material3.Text
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextInput
import androidx.compose.ui.test.performTextReplacement
import androidx.test.espresso.intent.Intents
import androidx.test.espresso.intent.matcher.IntentMatchers.hasAction
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.BeforeClass
import org.junit.Rule
import org.junit.Test
import org.usbmem.app.ui.AppModel
import org.usbmem.app.ui.DiskRange
import org.usbmem.app.ui.DiskSlot
import org.usbmem.app.ui.DiskTarget
import org.usbmem.app.ui.DiskTargetKind
import org.usbmem.app.ui.DiskToolsFeatures
import org.usbmem.app.ui.DiskToolsNav
import org.usbmem.app.ui.UsbDeviceState
import java.io.File
import java.io.FileInputStream
import java.io.RandomAccessFile
import java.security.MessageDigest

/**
 * ディスクツール (検査 / フォーマット / パーティション編集 / バックアップ・復元 / 救出) の画面を、実画面 (Compose) の操作で通す試験。docs/disk-ui.md
 * イメージ画面で開いたイメージ (ファイル選択だけ espresso-intents で差し替え) と、偽の USB セッション (実イメージを native のデバイスとして渡す) に対して行う。
 * 書き込んだイメージは cacheDir/dui_out に残し、CI (ci_android_tests.sh の disk-ui グループ) が adb で取り出して、
 * e2fsck / fsck.fat / fsck.exfat / sgdisk / cmp で外から検証する (どのファイルに何を確かめるかは run_disk_ui)。
 * フィクスチャは tools/gen_disk_ui_fixture.py (/data/local/tmp/dui_*)。
 */
class DiskUiTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val usbName = "/dev/bus/usb/001/077"

    companion object {
        /** 試験クラスの最初に 1 度だけ、前回の出力を消す (試験ごとに別の名前で書き、全部が終わってから CI が取り出す)。 */
        @BeforeClass @JvmStatic fun wipeOutputs() {
            File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "dui_out").deleteRecursively()
        }
    }

    private fun s(id: Int) = ctx.getString(id)
    private fun shell(cmd: String): ByteArray {
        val pfd = inst.uiAutomation.executeShellCommand(cmd)
        return FileInputStream(pfd.fileDescriptor).use { it.readBytes() }.also { pfd.close() }
    }

    private val outDir: File get() = File(ctx.cacheDir, "dui_out").also { it.mkdirs() }

    /** /data/local/tmp/dui_<name> を cacheDir/dui_out/<as> へ写す (元は変えない)。 */
    private fun fixture(name: String, asName: String = name): File {
        val f = File(outDir, asName)
        f.delete()
        val pfd = inst.uiAutomation.executeShellCommand("cat /data/local/tmp/dui_$name")
        FileInputStream(pfd.fileDescriptor).use { i -> f.outputStream().use { i.copyTo(it) } }
        pfd.close()
        assumeTrue("fixture dui_$name not pushed", f.length() > 0)
        return f
    }

    /** 空の (ゼロの疎な) イメージ。 */
    private fun blank(name: String, mib: Int): File {
        val f = File(outDir, name)
        f.delete()
        RandomAccessFile(f, "rw").use { it.setLength(mib.toLong() shl 20) }
        return f
    }

    private fun sha(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        FileInputStream(f).use { ins -> val buf = ByteArray(1 shl 20); while (true) { val n = ins.read(buf); if (n < 0) break; md.update(buf, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    @Before fun setUp() { Intents.init() }

    @After fun tearDown() {
        runCatching { Intents.release() }
        DiskSlot.entries.forEach { DiskToolsFeatures.unregister(it) }
        rule.runOnUiThread { DiskToolsNav.close(); AppModel.shared.usbDevices = emptyList() }
        UsbSessions.testOverride?.closeAll()
        UsbSessions.testOverride = null
        shell("settings put system font_scale 1.0")
        inst.uiAutomation.setRotation(UiAutomation.ROTATION_UNFREEZE)
    }

    // ---- 画面の操作
    private fun tag(t: String) = rule.onNodeWithTag(t)
    private fun exists(t: String) = rule.onAllNodesWithTag(t).fetchSemanticsNodes().isNotEmpty()
    private fun click(n: SemanticsNodeInteraction) { try { n.performScrollTo() } catch (_: AssertionError) { }; n.performClick() }
    private fun clickTag(t: String) = click(rule.onAllNodesWithTag(t)[0])
    private fun clickText(t: String) = click(rule.onNodeWithText(t))
    private fun waitTag(t: String, ms: Long = 120_000) { rule.waitUntil(ms) { exists(t) } }
    private fun waitGone(t: String, ms: Long = 120_000) { rule.waitUntil(ms) { !exists(t) } }
    private fun typeInto(t: String, text: String) { val n = rule.onAllNodesWithTag(t)[0]; try { n.performScrollTo() } catch (_: AssertionError) { }; n.performTextReplacement(text) }
    /** [t] のボタンが押せる (無効でない) ようになるまで待つ。 */
    private fun waitEnabled(t: String, ms: Long = 120_000) { rule.waitUntil(ms) { rule.onAllNodesWithTag(t).fetchSemanticsNodes().firstOrNull()?.config?.let { SemanticsProperties.Disabled !in it } == true } }
    private fun textOf(t: String): String = tag(t).fetchSemanticsNode().config[SemanticsProperties.Text].joinToString("") { it.text }

    /** 次のファイル選択 (開く / 作る) を、指定のファイルを選んだことにする。null の側は差し替えない。差し替えのたびに登録し直す。 */
    private fun stubs(open: File? = null, create: File? = null) {
        Intents.release(); Intents.init()
        if (open != null) Intents.intending(hasAction(Intent.ACTION_OPEN_DOCUMENT)).respondWith(Instrumentation.ActivityResult(Activity.RESULT_OK, Intent().setData(Uri.fromFile(open))))
        if (create != null) Intents.intending(hasAction(Intent.ACTION_CREATE_DOCUMENT)).respondWith(Instrumentation.ActivityResult(Activity.RESULT_OK, Intent().setData(Uri.fromFile(create))))
    }

    /** イメージ画面でイメージを開き、「ディスクツール」へ入る (一覧まで)。[writable] = 「書き込みを許可」を入れる。 */
    private fun openImageTools(f: File, writable: Boolean, create: File? = null) {
        stubs(open = f, create = create)
        clickText(s(R.string.tab_image))
        if (writable) clickTag("image_write_switch")
        clickText(s(R.string.image_open_raw))
        waitTag("disk_tools_image")
        clickTag("disk_tools_image")
        waitTag("disk_tools_host")
        waitTag("disk_tool_check")
    }

    private fun openTool(t: String) { clickTag(t); waitTag("disk_title") }
    private fun back() { clickTag("disk_back") }

    private fun phrase(): String { waitTag("disk_confirm_phrase"); return textOf("disk_confirm_phrase") }
    private fun typeAndConfirm() {
        val p = phrase()
        tag("disk_confirm_ok").assertIsNotEnabled()
        tag("disk_confirm_input").performTextInput(p)
        tag("disk_confirm_ok").assertIsEnabled()
        clickTag("disk_confirm_ok")
    }

    private fun tableOf(f: File): PartitionTable {
        val pfd = ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
        val h = NativeBridge.nativeOpenFd(pfd.detachFd(), true)
        check(Handle.valid(h))
        try {
            val err = IntArray(1)
            val b = NativeBridge.nativeListPartitions(h, err) ?: error("list failed ${err[0]}")
            return PartitionTable.parse(String(b, Charsets.UTF_8))
        } finally { NativeBridge.nativeClose(h) }
    }

    // ================================================================ 検査 (読み取り専用)

    @Test fun check_cleanImage_showsCleanVerdict_andWritesNothing() {
        val img = fixture("ext2_clean.img", "chk_clean.img")
        val before = sha(img)
        openImageTools(img, writable = false)
        openTool("disk_tool_check")
        tag("tool_header_readonly").assertExists()
        assertFalse("a read-only tool must not show the destructive heading", exists("tool_header_destructive"))
        tag("check_limits").assertExists()
        clickTag("check_run")
        waitTag("check_result")
        tag("check_verdict_clean").assertExists()
        tag("check_no_write").assertExists()
        assertEquals("the check must not write", before, sha(img))
    }

    @Test fun check_badImage_listsFindings() {
        val img = fixture("ext2_bad.img", "chk_bad.img")
        val before = sha(img)
        openImageTools(img, writable = false)
        openTool("disk_tool_check")
        clickTag("check_run")
        waitTag("check_result")
        assertFalse("a damaged file system must not be reported clean", exists("check_verdict_clean"))
        tag("check_count_errors").assertExists()
        tag("check_findings").assertExists()
        assertEquals(before, sha(img))
    }

    // ================================================================ フォーマット

    private fun formatOnBlank(fs: String, outName: String) {
        val img = blank(outName, 40)
        openImageTools(img, writable = true)
        openTool("disk_tool_format")
        tag("tool_header_destructive").assertExists()
        waitTag("fmt_fs_$fs")
        clickTag("fmt_fs_$fs")
        typeInto("fmt_label", "DUIFMT")
        clickTag("fmt_plan")
        waitTag("fmt_plan_card")
        clickTag("fmt_apply")
        typeAndConfirm()
        waitTag("fmt_done")
        tag("fmt_verify").assertExists()
    }

    @Test fun format_ext2_onBlankImage() = formatOnBlank("ext2", "fmt_ext2.img")
    @Test fun format_exfat_onBlankImage() = formatOnBlank("exfat", "fmt_exfat.img")
    @Test fun format_fat32_onBlankImage() = formatOnBlank("fat32", "fmt_fat32.img")

    @Test fun format_cancelConfirm_changesNothing() {
        val img = blank("fmt_cancel.img", 40)
        val before = sha(img)
        openImageTools(img, writable = true)
        openTool("disk_tool_format")
        waitTag("fmt_fs_ext2")
        clickTag("fmt_plan")
        waitTag("fmt_plan_card")
        clickTag("fmt_apply")
        waitTag("disk_confirm_phrase")
        tag("disk_confirm_subject").assertExists()
        tag("disk_confirm_ok").assertIsNotEnabled()
        tag("disk_confirm_input").performTextInput("WRONG")
        tag("disk_confirm_ok").assertIsNotEnabled()
        clickTag("disk_confirm_cancel")
        waitGone("disk_confirm_phrase")
        assertFalse(exists("fmt_done"))
        assertEquals("cancelling the confirmation must not touch the image", before, sha(img))
    }

    @Test fun format_readOnly_cannotApply() {
        val img = fixture("ext2_clean.img", "fmt_ro.img")
        val before = sha(img)
        openImageTools(img, writable = false)
        openTool("disk_tool_format")
        tag("fmt_need_write").assertExists()
        waitTag("fmt_fs_ext2")
        clickTag("fmt_plan")
        rule.waitUntil(120_000) { exists("fmt_plan_card") || exists("fmt_plan_error") }
        if (exists("fmt_apply")) tag("fmt_apply").assertIsNotEnabled()
        assertEquals(before, sha(img))
    }

    // ================================================================ パーティション編集

    @Test fun partition_viewShowsLayout_slotsHiddenUntilRegistered() {
        val img = fixture("gpt2.img", "pt_view.img")
        openImageTools(img, writable = false)
        openTool("disk_tool_partition")
        tag("tool_header_readonly").assertExists()
        waitTag("pt_bar")
        tag("pt_legend").assertExists()
        tag("pt_row_1").assertExists(); tag("pt_row_2").assertExists()
        assertFalse("an unregistered feature slot must stay hidden", exists("disk_slot_lost_partitions"))
        assertFalse(exists("disk_slot_boot_sector"))
        rule.runOnUiThread { DiskToolsFeatures.register(DiskSlot.LostPartitions) { Text("slot-body") } }
        waitTag("disk_slot_lost_partitions")
        assertFalse(exists("disk_slot_boot_sector"))
        assertTrue(DiskToolsFeatures.available(DiskSlot.LostPartitions))
        rule.runOnUiThread { DiskToolsFeatures.unregister(DiskSlot.LostPartitions) }
        waitGone("disk_slot_lost_partitions")
    }

    @Test fun partition_remove_planShowsBeforeAfter_thenApplies() {
        val img = fixture("gpt2.img", "pt_remove.img")
        openImageTools(img, writable = true)
        openTool("disk_tool_partition")
        waitTag("pt_row_2")
        clickTag("pt_row_2")
        clickTag("pt_remove_2")
        waitTag("pt_ops")
        tag("pt_op_0").assertExists()
        clickTag("pt_plan")
        waitTag("pt_plan_card")
        tag("pt_before_bar").assertExists(); tag("pt_after_bar").assertExists()
        tag("pt_before").assertExists(); tag("pt_after").assertExists()
        tag("pt_destructive").assertExists()
        clickTag("pt_apply")
        typeAndConfirm()
        waitTag("pt_done")
        val t = tableOf(img)
        assertEquals("one partition must remain", 1, t.partitions.count { !it.protective })
    }

    @Test fun partition_cancelConfirm_changesNothing() {
        val img = fixture("gpt2.img", "pt_cancel.img")
        val before = sha(img)
        openImageTools(img, writable = true)
        openTool("disk_tool_partition")
        waitTag("pt_row_2")
        clickTag("pt_row_2")
        clickTag("pt_remove_2")
        clickTag("pt_plan")
        waitTag("pt_plan_card")
        clickTag("pt_apply")
        waitTag("disk_confirm_phrase")
        clickTag("disk_confirm_cancel")
        waitGone("disk_confirm_phrase")
        assertFalse(exists("pt_done"))
        assertEquals("cancelling the confirmation must not touch the image", before, sha(img))
    }

    @Test fun partition_readOnly_cannotApply() {
        val img = fixture("gpt2.img", "pt_ro.img")
        val before = sha(img)
        openImageTools(img, writable = false)
        openTool("disk_tool_partition")
        tag("pt_need_write").assertExists()
        waitTag("pt_row_2")
        clickTag("pt_row_2")
        clickTag("pt_remove_2")
        clickTag("pt_plan")
        rule.waitUntil(120_000) { exists("pt_plan_card") || exists("pt_plan_error") }
        if (exists("pt_apply")) tag("pt_apply").assertIsNotEnabled()
        assertEquals(before, sha(img))
    }

    // ================================================================ バックアップ / 復元

    @Test fun backup_runsAndVerifies() {
        val img = fixture("ext2_clean.img", "bak_src.img")
        val dest = File(outDir, "bak_out.img").also { it.delete(); it.createNewFile() }
        val before = sha(img)
        openImageTools(img, writable = false, create = dest)
        openTool("disk_tool_backup")
        tag("tool_header_readonly").assertExists()
        clickTag("bak_pick_dest")
        waitTag("bak_dest_name")
        clickTag("bak_run")
        waitTag("bak_done")
        assertTrue("the backup file must have content", dest.length() > 0)
        clickTag("bak_verify")
        waitTag("bak_verified")
        assertEquals("a backup must not write the source", before, sha(img))
    }

    @Test fun restore_showsSideBySide_cancelChangesNothing_thenRestores() {
        val src = fixture("ext2_clean.img", "res_src.img")
        val tgt = fixture("ext2_target.img", "res_target.img")
        // 復元元のバックアップは、画面の外 (コントローラ直接) で作る。画面の復元の手順だけをここで通す
        val bak = File(outDir, "res_backup.img").also { it.delete(); it.createNewFile() }
        var made: org.usbmem.app.ui.Outcome<org.json.JSONObject>? = null
        val th = Thread {
            val d = rule.activity.diskTools
            val job = d.newRecoveryJob()
            try { made = d.backupRun(DiskTarget(DiskTargetKind.Image, Uri.fromFile(src).toString(), "src"), DiskRange(0, src.length(), "", "ext2", 0), Uri.fromFile(bak), true, false, job) } finally { job.close() }
        }
        th.start(); th.join()
        assertTrue("backup setup failed: ${made?.error}", made?.value?.optBoolean("ok") == true)

        val before = sha(tgt)
        openImageTools(tgt, writable = true)
        openTool("disk_tool_backup")
        assertFalse("opened writable: no write-mode notice", exists("res_need_write"))
        stubs(open = bak)
        clickTag("res_pick_src")
        waitTag("res_src_name")
        clickTag("res_plan")
        waitTag("res_plan_card")
        tag("res_compare").assertExists()
        tag("res_cmp_img_fs").assertExists(); tag("res_cmp_tgt_fs").assertExists()
        tag("res_cmp_tgt_label").assertExists()
        assertFalse("same file system: no mismatch warning", exists("res_fs_mismatch"))
        // 取り消しでは何も変わらない
        clickTag("res_apply")
        waitTag("disk_confirm_phrase")
        tag("disk_confirm_ok").assertIsNotEnabled()
        clickTag("disk_confirm_cancel")
        waitGone("disk_confirm_phrase")
        assertEquals(before, sha(tgt))
        // 実行
        clickTag("res_apply")
        typeAndConfirm()
        waitTag("res_done")
    }

    // ================================================================ 救出

    private fun randomImage(name: String, mib: Int): File {
        val f = File(outDir, name)
        f.delete()
        val r = java.util.Random(42)
        val buf = ByteArray(1 shl 20)
        f.outputStream().use { o -> repeat(mib) { r.nextBytes(buf); o.write(buf) } }
        return f
    }

    @Test fun rescue_preflight_cancel_resume_finishes() {
        val src = randomImage("rescue_src.img", 32)
        val out = File(outDir, "rescue_out.img").also { it.delete(); it.createNewFile() }
        rule.activity.diskTools.rescueResetMap(DiskTarget(DiskTargetKind.Image, Uri.fromFile(src).toString(), "src"), DiskRange(0, src.length(), "", "", 0), Uri.fromFile(out))
        openImageTools(src, writable = false, create = out)
        openTool("disk_tool_rescue")
        tag("tool_header_readonly").assertExists()
        clickTag("rescue_pick_out")
        waitTag("rescue_out_name")
        waitTag("rescue_pre_ok")                      // 開始前の検査の結果が、開始の前に見える
        tag("rescue_preflight").assertExists()
        tag("rescue_run").assertIsEnabled()
        typeInto("rescue_pause", "100")
        waitEnabled("rescue_run")
        clickTag("rescue_run")
        waitTag("rescue_band")                        // 走り出して、進捗の帯が出た
        clickTag("rescue_pause_btn"); clickTag("rescue_pause_btn")   // 一時停止 → 再開 (押せる)
        clickTag("rescue_cancel")
        waitTag("rescue_stop_cancelled")
        tag("rescue_resumable").assertExists()
        // 進捗ファイルが残り、検査は「続きから」になる
        waitTag("rescue_pre_band")
        // 続きから、間隔なしで最後まで
        typeInto("rescue_pause", "0")
        waitEnabled("rescue_run")
        clickTag("rescue_run")
        waitTag("rescue_stop_finished")
        tag("rescue_result_band").assertExists()
        assertEquals("the rescued image must equal the source", sha(src), sha(out))
    }

    @Test fun rescue_sameFileAsSource_isRefused() {
        val src = randomImage("rescue_same.img", 2)
        val before = sha(src)
        openImageTools(src, writable = false)
        openTool("disk_tool_rescue")
        stubs(open = src)
        clickTag("rescue_pick_existing")
        waitTag("rescue_out_name")
        waitTag("rescue_pre_error")
        tag("rescue_run").assertIsNotEnabled()
        assertEquals("the source must be untouched", before, sha(src))
    }

    // ================================================================ USB (偽のセッション。実イメージを native のデバイスとして渡す)

    private class FakeSession(override val deviceName: String, override val writable: Boolean, private val image: File) : UsbSession {
        @Volatile var closed = false
        override val vendorId = 0x0781; override val productId = 0x5581; override val tuningIndex = -1
        override fun partitionTable(): PartitionTable = withHandle { h ->
            val err = IntArray(1)
            PartitionTable.parse(String(NativeBridge.nativeListPartitions(h, err) ?: error("list ${err[0]}"), Charsets.UTF_8))
        }
        override fun opStats() = ""
        override fun mountNtfs(offsetBytes: Long, sizeBytes: Long, writable: Boolean, createOnly: Boolean, allowNoload: Boolean, allowDirty: Boolean): NtfsVolume = error("not used")
        override fun startStop(start: Boolean) {}
        override fun isSpunDown() = false
        override fun setIdleSpinDown(idleMinutes: Int, onStopped: (Boolean) -> Unit) {}
        override fun tune(chunkKiB: Int, maxTransferKiB: Int) {}
        override fun benchReadHash(offset: Long, totalBytes: Long, bufKiB: Int, hash: LongArray) = 1L
        override fun benchRead(offset: Long, totalBytes: Long, bufKiB: Int) = 1L
        override fun <T> withHandle(block: (Long) -> T): T {
            check(!closed)
            val pfd = ParcelFileDescriptor.open(image, if (writable) ParcelFileDescriptor.MODE_READ_WRITE else ParcelFileDescriptor.MODE_READ_ONLY)
            val h = NativeBridge.nativeOpenFd(pfd.detachFd(), !writable)
            check(Handle.valid(h)) { "open failed" }
            try { return block(h) } finally { NativeBridge.nativeClose(h) }
        }
        override fun close() { closed = true }
    }

    private fun openFakeUsb(img: File, mode: WriteMode): UsbDeviceState {
        val mgr = UsbSessionManager(
            object : UsbSessionFactory { override fun open(device: UsbDeviceInfo, writable: Boolean): UsbSession = FakeSession(device.deviceName, writable, img) },
            onDeviceClosing = { name -> try { VolumeRegistry.removeByDevice(name) } finally { VolumeRegistry.notifyRoots(ctx) } },
        )
        UsbSessions.testOverride = mgr
        assertTrue(mgr.setWriteMode(usbName, mode))
        mgr.open(UsbDeviceInfo(usbName, "Fake Stick", 0x0781, 0x5581))
        val table = tableOf(img)
        val st = UsbDeviceState(usbName).also {
            it.title = "Fake Stick"; it.open = true; it.writable = mode != WriteMode.ReadOnly; it.writeMode = mode; it.table = table
        }
        rule.runOnUiThread { AppModel.shared.usbDevices = listOf(st) }
        return st
    }

    @Test fun usb_readOnlySession_checkWorks_butWriteToolsAreBlocked() {
        val img = fixture("ext2_clean.img", "usb_ro.img")
        val before = sha(img)
        openFakeUsb(img, WriteMode.ReadOnly)
        waitTag("disk_tools_usb")
        clickTag("disk_tools_usb")
        waitTag("disk_hub_readonly_notice")
        openTool("disk_tool_check")
        clickTag("check_run")
        waitTag("check_result")
        tag("check_verdict_clean").assertExists()
        back()
        openTool("disk_tool_format")
        tag("fmt_need_write").assertExists()
        assertEquals(before, sha(img))
    }

    @Test fun usb_fullSession_formatsExt2() {
        assumeTrue("write support disabled in this build", BuildConfig.WRITE_SUPPORTED)
        val img = blank("usb_fmt.img", 40)
        openFakeUsb(img, WriteMode.Full)
        waitTag("disk_tools_usb")
        clickTag("disk_tools_usb")
        waitTag("disk_tool_format")
        openTool("disk_tool_format")
        assertFalse(exists("fmt_need_write"))
        waitTag("fmt_fs_ext2")
        clickTag("fmt_fs_ext2")
        clickTag("fmt_plan")
        waitTag("fmt_plan_card")
        clickTag("fmt_apply")
        typeAndConfirm()
        waitTag("fmt_done")
    }

    // ================================================================ 横向き / 大きい文字

    private fun inLandscape(block: () -> Unit) {
        val ua = inst.uiAutomation
        ua.setRotation(UiAutomation.ROTATION_FREEZE_90)
        try {
            rule.waitUntil(30_000) { rule.activity.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE }
            rule.waitForIdle()
            block()
        } finally {
            ua.setRotation(UiAutomation.ROTATION_FREEZE_0)
            ua.setRotation(UiAutomation.ROTATION_UNFREEZE)
        }
    }

    private fun openFormatConfirm(name: String): File {
        val img = blank(name, 40)
        openImageTools(img, writable = true)
        openTool("disk_tool_format")
        waitTag("fmt_fs_ext2")
        clickTag("fmt_plan")
        waitTag("fmt_plan_card")
        clickTag("fmt_apply")
        waitTag("disk_confirm_phrase")
        return img
    }

    @Test fun confirmDialog_inLandscape_staysUsable() {
        val img = openFormatConfirm("land_fmt.img")
        val before = sha(img)
        inLandscape {
            tag("disk_confirm_cancel").assertExists()
            click(tag("disk_confirm_input"))
            tag("disk_confirm_input").performTextInput(phrase())
            tag("disk_confirm_ok").assertIsEnabled()
        }
        clickTag("disk_confirm_cancel")
        waitGone("disk_confirm_phrase")
        assertEquals(before, sha(img))
    }

    @Test fun confirmDialog_withLargeFont_staysUsable() {
        val img = openFormatConfirm("font_fmt.img")
        val before = sha(img)
        shell("settings put system font_scale 2.0")
        rule.waitUntil(30_000) { rule.activity.resources.configuration.fontScale >= 1.9f }
        rule.waitForIdle()
        waitTag("disk_confirm_phrase")
        tag("disk_confirm_cancel").assertExists()
        click(tag("disk_confirm_input"))
        tag("disk_confirm_input").performTextInput(phrase())
        tag("disk_confirm_ok").assertIsEnabled()
        clickTag("disk_confirm_cancel")
        waitGone("disk_confirm_phrase")
        assertEquals(before, sha(img))
    }

    @Test fun hub_largeFont_listsAllToolsAndSeparatesWriteGroup() {
        val img = fixture("ext2_clean.img", "hub_font.img")
        openImageTools(img, writable = false)
        shell("settings put system font_scale 2.0")
        rule.waitUntil(30_000) { rule.activity.resources.configuration.fontScale >= 1.9f }
        rule.waitForIdle()
        for (t in listOf("disk_tool_check", "disk_tool_rescue", "disk_tool_format", "disk_tool_partition", "disk_tool_backup")) tag(t).assertExists()
        assertNotNull(DiskToolsNav.target)
    }
}
