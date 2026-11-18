package org.usbmem.app

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.app.UiAutomation
import android.content.res.Configuration
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertIsOff
import androidx.compose.ui.test.assertIsOn
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.performSemanticsAction
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
import org.junit.Rule
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.KeyStore
import java.security.MessageDigest
import javax.crypto.Cipher

/**
 * 暗号ボリューム (LUKS2 / BitLocker) の UI を、実画面 (Compose) の操作で通す試験。docs/crypto-volumes.md 10 章。
 * イメージ画面の「RAW イメージを開く」→ パーティション一覧の「解錠してマウント」→ 解錠ダイアログ (パスワード・回復キー・BEK) の流れを、
 * ファイル選択だけ espresso-intents で差し替えて (file URI を返す) 実行する。結果の確認は SAF (ContentResolver / DocumentsContract) で行う。
 * フィクスチャは tools/gen_crypt_ui_fixture.py (CI: ci_android_tests.sh の crypt-ui グループが /data/local/tmp/cui_* へ送る)。
 */
class CryptoUiTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = "org.usbmem.app.documents"
    private val cr get() = ctx.contentResolver
    private val pw = HashMap<String, String>()

    private fun s(id: Int) = ctx.getString(id)
    private fun shell(cmd: String): ByteArray {
        val pfd = inst.uiAutomation.executeShellCommand(cmd)
        return FileInputStream(pfd.fileDescriptor).use { it.readBytes() }.also { pfd.close() }
    }

    /** /data/local/tmp のフィクスチャをアプリの cacheDir へ写す (writable で開くので、元は変えない)。 */
    private fun fixture(name: String): File {
        val f = File(ctx.cacheDir, name)
        f.delete()
        val pfd = inst.uiAutomation.executeShellCommand("cat /data/local/tmp/cui_$name")
        FileInputStream(pfd.fileDescriptor).use { i -> f.outputStream().use { i.copyTo(it) } }
        pfd.close()
        assumeTrue("fixture cui_$name not pushed", f.length() > 0)
        return f
    }

    @Before fun setUp() {
        val txt = shell("cat /data/local/tmp/cui_pw.ref_creds").toString(Charsets.UTF_8)
        assumeTrue("fixture cui_pw.ref_creds not pushed", txt.isNotBlank())
        txt.lines().filter { '=' in it }.forEach { pw[it.substringBefore('=')] = it.substringAfter('=') }
        Intents.init()
    }

    /** 次にファイル選択 (ACTION_OPEN_DOCUMENT) が開かれたら、[f] を選んだことにする (差し替えのたびに登録し直す)。 */
    private fun stubPick(f: File) {
        Intents.release(); Intents.init()
        Intents.intending(hasAction(Intent.ACTION_OPEN_DOCUMENT)).respondWith(Instrumentation.ActivityResult(Activity.RESULT_OK, Intent().setData(Uri.fromFile(f))))
    }

    @After fun tearDown() {
        Intents.release()
        VaultProvider.override = null
        cleanVaultState()
        // 解錠したまま残らないように (他の試験・次の試験へ持ち越さない)
        VolumeRegistry.discardAllPendingCrypto()
        VolumeRegistry.cryptoEntries().map { it.id }.distinct().forEach { runCatching { VolumeRegistry.remove(ctx, it) } }
    }

    // ---- 画面の操作
    private fun tag(t: String) = rule.onNodeWithTag(t)
    /** スクロールできる所ならスクロールして押す (ダイアログのボタンなど、スクロール親が無い所はそのまま押す)。 */
    private fun click(n: SemanticsNodeInteraction) { try { n.performScrollTo() } catch (_: AssertionError) { }; n.performClick() }
    private fun clickTag(t: String, index: Int = 0) = click(rule.onAllNodesWithTag(t)[index])
    private fun clickText(t: String) = click(rule.onNodeWithText(t))
    private fun waitTag(t: String, count: Int = 1, ms: Long = 120_000) { rule.waitUntil(ms) { rule.onAllNodesWithTag(t).fetchSemanticsNodes().size >= count } }
    private fun waitGone(t: String, ms: Long = 120_000) { rule.waitUntil(ms) { rule.onAllNodesWithTag(t).fetchSemanticsNodes().isEmpty() } }
    private fun waitText(t: String, ms: Long = 120_000) { rule.waitUntil(ms) { rule.onAllNodesWithText(t, substring = true).fetchSemanticsNodes().isNotEmpty() } }

    private fun openImage(f: File) {
        stubPick(f)
        clickText(s(R.string.tab_image))
        clickText(s(R.string.image_open_raw))
        waitTag("crypto_unlock_button")
    }

    private fun sha(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    // ---- SAF
    private fun rootIdByTitle(title: String): String? =
        cr.query(DocumentsContract.buildRootsUri(auth), arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_TITLE), null, null, null)!!.use { c ->
            var r: String? = null
            while (c.moveToNext()) if (c.getString(1) == title) r = c.getString(0)
            r
        }
    private fun rootFlags(id: String): Int = cr.query(DocumentsContract.buildRootsUri(auth), arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS), null, null, null)!!.use { c ->
        var r = -1
        while (c.moveToNext()) if (c.getString(0) == id) r = c.getInt(1)
        r
    }
    private fun rootSummary(id: String): String = cr.query(DocumentsContract.buildRootsUri(auth), arrayOf(Root.COLUMN_ROOT_ID, Root.COLUMN_SUMMARY), null, null, null)!!.use { c ->
        var r = ""
        while (c.moveToNext()) if (c.getString(0) == id) r = c.getString(1) ?: ""
        r
    }
    private fun docUri(id: String, path: String) = DocumentsContract.buildDocumentUri(auth, "$id:$path")
    private fun names(id: String, path: String): Set<String> {
        val out = HashSet<String>()
        cr.query(DocumentsContract.buildChildDocumentsUri(auth, "$id:$path"), arrayOf(Document.COLUMN_DISPLAY_NAME), null, null, null)!!.use { while (it.moveToNext()) out += it.getString(0) }
        return out
    }
    private fun readAll(id: String, path: String): ByteArray = cr.openInputStream(docUri(id, path))!!.use { it.readBytes() }
    private fun waitRoot(title: String): String {
        rule.waitUntil(60_000) { rootIdByTitle(title) != null }
        return rootIdByTitle(title)!!
    }
    private fun waitRootGone(title: String) { rule.waitUntil(60_000) { rootIdByTitle(title) == null } }

    /** 解錠ダイアログで、パスワードを入れて送る (ダイアログは開いている前提)。 */
    private fun typePasswordAndSubmit(p: String) {
        tag("unlock_password").performTextReplacement(p)
        tag("unlock_submit").assertIsEnabled().performClick()
    }

    /** 解錠が終わる (ダイアログが閉じる) まで待つ。 */
    private fun waitUnlocked() { waitGone("unlock_dialog") }

    private fun assertNoSecretLeak() {
        val log = shell("logcat -d").toString(Charsets.UTF_8)
        val files = File(ctx.dataDir, "shared_prefs").listFiles()?.joinToString("\n") { it.readText() } ?: ""
        for ((k, v) in pw) {
            assertFalse("password $k found in logcat", log.contains(v))
            assertFalse("password $k found in shared_prefs", files.contains(v))
        }
    }

    // ================================================================================================

    /** GPT のイメージ: LUKS2 (ext4) と BitLocker (NTFS)。間違ったパスワード → 再入力 → 読み取り専用で解錠 → 一覧 → 書き込み可で解錠して SAF で書く → ロック → 開き直して検証。 */
    @Test
    fun gptImage_luks2WrongPasswordThenUnlockWriteAndVerify() {
        openImage(fixture("gpt.img"))
        waitTag("crypto_unlock_button", 2)
        val title = "gpt.img #1"

        // ---- 1. 解錠ダイアログ: 既定はパスワード欄がマスクされ、書き込みは切 / 間違ったパスワードは再入力できる
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        tag("unlock_password").performTextInput("definitely-wrong-password")
        tag("unlock_password").assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.Password))     // マスク表示
        clickTag("unlock_show_toggle")
        tag("unlock_password").assert(SemanticsMatcher.keyNotDefined(SemanticsProperties.Password))   // 表示に切り替え
        clickTag("unlock_show_toggle")
        tag("unlock_password").assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.Password))
        tag("unlock_submit").assertIsEnabled().performClick()
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.unlock_err_password)).assertExists()    // 認証失敗の明確な文言 (ダイアログは開いたまま)
        tag("unlock_dialog").assertExists()
        assertEquals(null, rootIdByTitle(title))
        // 入力欄は送信で空になっている (前回の内容が残らない)
        tag("unlock_submit").assertIsNotEnabled()

        // ---- 2. 正しいパスワード → 読み取り専用で解錠 (既定)
        typePasswordAndSubmit(pw.getValue("cui_gpt_luks"))
        waitUnlocked()
        val id = waitRoot(title)
        assertEquals("読み取り専用のはず: ${rootSummary(id)}", 0, rootFlags(id) and Root.FLAG_SUPPORTS_CREATE)
        assertTrue(rootSummary(id), rootSummary(id).contains("暗号化"))
        val n = names(id, "/")
        assertTrue(n.toString(), "seed.txt" in n && "dir" in n && "f_4096.bin" in n)
        assertEquals("volume seed 5001\n", readAll(id, "/seed.txt").toString(Charsets.UTF_8))
        waitTag("crypto_lock")

        // ---- 3. ロック (アンマウント)
        clickTag("crypto_lock")
        waitRootGone(title)
        waitGone("crypto_lock")

        // ---- 4. 書き込み可で解錠: 切り替えは確認ダイアログを経る (実験的の警告)
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        tag("unlock_write_switch").performClick()
        rule.onNodeWithText(s(R.string.unlock_write_confirm_text)).assertExists()
        clickText(s(R.string.unlock_write_confirm))
        typePasswordAndSubmit(pw.getValue("cui_gpt_luks"))
        waitUnlocked()
        val idw = waitRoot(title)
        assertTrue("書き込み可のはず: ${rootSummary(idw)}", rootFlags(idw) and Root.FLAG_SUPPORTS_CREATE != 0)
        val payload = ByteArray(300_000) { ((it * 31) xor (it shr 8)).toByte() }
        val doc = DocumentsContract.createDocument(cr, docUri(idw, "/"), "application/octet-stream", "ui_written.bin")
        assertNotNull(doc)
        cr.openOutputStream(doc!!, "w")!!.use { it.write(payload) }
        val dir = DocumentsContract.createDocument(cr, docUri(idw, "/"), Document.MIME_TYPE_DIR, "ui_dir")
        assertNotNull(dir)
        val doc2 = DocumentsContract.createDocument(cr, dir!!, "text/plain", "メモ.txt")
        assertNotNull(doc2)
        cr.openOutputStream(doc2!!, "w")!!.use { it.write("暗号ボリュームに書きました\n".toByteArray()) }
        assertEquals(sha(payload), sha(readAll(idw, "/ui_written.bin")))
        clickTag("crypto_lock")
        waitRootGone(title)

        // ---- 5. 開き直して (読み取り専用) 検証: 書いた内容が暗号の下まで届いている
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        typePasswordAndSubmit(pw.getValue("cui_gpt_luks"))
        waitUnlocked()
        val id2 = waitRoot(title)
        assertEquals(sha(payload), sha(readAll(id2, "/ui_written.bin")))
        assertEquals("暗号ボリュームに書きました\n", readAll(id2, "/ui_dir/メモ.txt").toString(Charsets.UTF_8))
        assertEquals("volume seed 5001\n", readAll(id2, "/seed.txt").toString(Charsets.UTF_8))   // 元の内容も無事
        clickTag("crypto_lock")
        waitRootGone(title)

        // ---- 6. 2 つ目のパーティション: BitLocker (NTFS)。パスワード方式しか無いので方式の選択は出ない
        clickTag("crypto_unlock_button", 1)
        waitTag("unlock_dialog")
        rule.onAllNodesWithText(s(R.string.unlock_method_recovery)).assertCountEquals(0)
        typePasswordAndSubmit(pw.getValue("cui_gpt_bl"))
        waitUnlocked()
        val idb = waitRoot("gpt.img #2")
        assertEquals("volume seed 5002\n", readAll(idb, "/seed.txt").toString(Charsets.UTF_8))
        clickTag("crypto_lock")
        waitRootGone("gpt.img #2")

        assertNoSecretLeak()
    }

    /** ディスク全体が LUKS2 で、中に GPT (ext4 + NTFS)。中のパーティションの開始位置でマウントされ、片方をロックすると両方が閉じる。 */
    @Test
    fun wholeDiskLuks_innerPartitionsMountedAtTheirOffsets() {
        openImage(fixture("whole.img"))
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        typePasswordAndSubmit(pw.getValue("cui_whole"))
        waitUnlocked()
        val a = waitRoot("whole.img #1")
        val b = waitRoot("whole.img #2")
        assertEquals("volume seed 5011\n", readAll(a, "/seed.txt").toString(Charsets.UTF_8))
        assertEquals("volume seed 5012\n", readAll(b, "/seed.txt").toString(Charsets.UTF_8))
        assertTrue("dir" in names(a, "/"))
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        waitRootGone("whole.img #2")    // 同じ解錠の兄弟も一緒に閉じる
        assertNoSecretLeak()
    }

    /** ディスク全体が BitLocker (パスワード / 回復キー / BEK): 方式の切り替え、回復キーの形式検査と認証失敗、BEK ファイル (SAF のピッカー) での解錠。 */
    @Test
    fun bitlockerRecoveryKeyAndBekFile() {
        openImage(fixture("bl.img"))
        val recovery = fixture("bl.ref_recovery").readText().trim()
        val bek = fixture("bl.ref_bek")
        val title = "bl.img"

        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        clickText(s(R.string.unlock_method_recovery))
        waitTag("unlock_recovery")
        // 形式が足りない間は送れない
        tag("unlock_recovery").performTextInput("123456")
        tag("unlock_submit").assertIsNotEnabled()
        // 形式は正しいが違う回復キー (最後の群を別の 11 の倍数に変える)
        val digits = recovery.filter { it.isDigit() }
        val wrong = digits.substring(0, 42) + (if (digits.substring(42) == "000011") "000022" else "000011")
        tag("unlock_recovery").performTextReplacement(wrong)
        tag("unlock_submit").assertIsEnabled().performClick()
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.unlock_err_recovery)).assertExists()
        // 正しい回復キー (ハイフン無しで入れる: 画面側で 6 桁区切りにそろえる)
        tag("unlock_recovery").performTextReplacement(digits)
        tag("unlock_submit").assertIsEnabled().performClick()
        waitUnlocked()
        val id = waitRoot(title)
        assertEquals("volume seed 5021\n", readAll(id, "/seed.txt").toString(Charsets.UTF_8))
        clickTag("crypto_lock")
        waitRootGone(title)

        // BEK ファイル: ピッカー (差し替え済み) で選ぶ
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        clickText(s(R.string.unlock_method_keyfile))
        waitTag("unlock_pick_bek")
        tag("unlock_submit").assertIsNotEnabled()
        stubPick(bek)
        clickTag("unlock_pick_bek")
        rule.waitUntil(30_000) { rule.onAllNodesWithText("bl.ref_bek", substring = true).fetchSemanticsNodes().isNotEmpty() }
        tag("unlock_submit").assertIsEnabled().performClick()
        waitUnlocked()
        val id2 = waitRoot(title)
        assertEquals("volume seed 5021\n", readAll(id2, "/seed.txt").toString(Charsets.UTF_8))
        clickTag("crypto_lock")
        waitRootGone(title)

        // パスワードの方式 (最初に選ばれているもの) でも解錠できる
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        typePasswordAndSubmit(pw.getValue("cui_bl"))
        waitUnlocked()
        waitRoot(title)
        clickTag("crypto_lock")
        waitRootGone(title)
        assertNoSecretLeak()
    }
    // ---- ジャーナル未再生 (needs_recovery) の ext4 を中に持つ暗号ボリューム: 「読み取り専用で開く」確認フロー (docs/crypto-volumes.md 10 章)

    private fun unlockWithPassword(key: String) {
        clickTag("crypto_unlock_button", 0)
        waitTag("unlock_dialog")
        typePasswordAndSubmit(pw.getValue(key))
        waitUnlocked()
    }

    private fun waitNoloadDialog() { waitText(s(R.string.noload_dialog_title)) }

    /** 解錠 → 確認ダイアログ (まだ何も公開されていない) → 「読み取り専用で開く」→ 読める・書き込み不可・ジャーナル無視 → ロック。 */
    @Test
    fun journalNeedsRecovery_confirmOpensReadOnly() {
        openImage(fixture("nl.img"))
        val title = "nl.img"
        unlockWithPassword("cui_nl")
        waitNoloadDialog()
        rule.onNodeWithText(s(R.string.noload_dialog_text), substring = true).assertExists()
        tag("dialog_subject").assertTextEquals("nl.img")   // どのボリュームの確認か (内部表記ではなくファイル名)
        // 確認するまでは何も公開されない。保留 (復号済みデバイスのハンドル) だけが残り、解錠済みの一覧には出ない
        assertEquals(null, rootIdByTitle(title))
        assertEquals(1, VolumeRegistry.pendingCryptoList().size)
        assertTrue(VolumeRegistry.cryptoEntries().isEmpty())
        clickText(s(R.string.noload_dialog_confirm))
        val id = waitRoot(title)
        assertEquals("読み取り専用のはず", 0, rootFlags(id) and Root.FLAG_SUPPORTS_CREATE)
        assertEquals("volume seed 5031\n", readAll(id, "/seed.txt").toString(Charsets.UTF_8))
        assertTrue("dir" in names(id, "/"))
        val e = VolumeRegistry.cryptoEntries().single()
        assertTrue("ジャーナルを無視して開いた印", e.volume.journalIgnored)
        assertFalse("書き込み不可", e.volume.writable)
        assertTrue(VolumeRegistry.pendingCryptoList().isEmpty())
        waitTag("crypto_lock")
        clickTag("crypto_lock")
        waitRootGone(title)
        assertTrue(VolumeRegistry.cryptoEntries().isEmpty())
        assertNoSecretLeak()
    }

    /** 確認ダイアログは画面の作り直し (回転相当) で出し直され、キャンセルすると公開されたものは無く、保留も消える (そのあと同じボリュームを解錠し直せる)。 */
    @Test
    fun journalNeedsRecovery_cancelLocksAndRecreateKeepsDialog() {
        openImage(fixture("nl.img"))
        val title = "nl.img"
        unlockWithPassword("cui_nl")
        waitNoloadDialog()
        assertEquals(1, VolumeRegistry.pendingCryptoList().size)

        // 画面の作り直し: 保留は登録簿にあるので、イメージ画面を開き直すとダイアログが戻る (鍵のハンドルは漏れも消えもしない)
        rule.activityRule.scenario.recreate()
        clickText(s(R.string.tab_image))
        waitNoloadDialog()
        assertEquals(1, VolumeRegistry.pendingCryptoList().size)

        // キャンセル: 何も公開せず、保留 (復号済みデバイス) を閉じる
        clickText(s(R.string.common_cancel))
        rule.waitUntil(30_000) { VolumeRegistry.pendingCryptoList().isEmpty() }
        waitText(s(R.string.crypto_noload_cancelled))
        assertEquals(null, rootIdByTitle(title))
        assertTrue(VolumeRegistry.cryptoEntries().isEmpty())

        // 状態が残っていない: もう一度開いて解錠すると、また確認が出て、今度は確認して開ける
        openImage(fixture("nl.img"))
        unlockWithPassword("cui_nl")
        waitNoloadDialog()
        clickText(s(R.string.noload_dialog_confirm))
        val id = waitRoot(title)
        assertEquals("volume seed 5031\n", readAll(id, "/seed.txt").toString(Charsets.UTF_8))
        clickTag("crypto_lock")
        waitRootGone(title)
        assertNoSecretLeak()
    }

    /** 横向き (高さが足りない) でも、確認ダイアログの本文は一番下までスクロールでき、ボタンは見えたまま。 */
    @Test
    fun journalNeedsRecovery_dialogScrollsInLandscape() {
        openImage(fixture("nl.img"))
        unlockWithPassword("cui_nl")
        waitNoloadDialog()
        val ua = inst.uiAutomation
        ua.setRotation(UiAutomation.ROTATION_FREEZE_90)
        try {
            rule.waitUntil(30_000) { rule.activity.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE }
            rule.waitForIdle()
            val n = tag("dialog_body")
            n.performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, 100_000f) }
            rule.waitForIdle()
            val r = n.fetchSemanticsNode().config[SemanticsProperties.VerticalScrollAxisRange]
            assertEquals("the dialog body must be scrollable to its end", r.maxValue(), r.value(), 1f)
            rule.onNodeWithText(s(R.string.noload_dialog_confirm)).assertIsDisplayed()
            rule.onNodeWithText(s(R.string.common_cancel)).assertIsDisplayed()
        } finally {
            ua.setRotation(UiAutomation.ROTATION_FREEZE_0)
            ua.setRotation(UiAutomation.ROTATION_UNFREEZE)
        }
        waitNoloadDialog()
        clickText(s(R.string.common_cancel))
        rule.waitUntil(30_000) { VolumeRegistry.pendingCryptoList().isEmpty() }
    }

    /** 中が GPT で、一部 (NTFS) は普通に開き、ジャーナル未再生の ext4 だけが確認待ちになる。確認すると両方が読め、ロックは両方を閉じる。 */
    @Test
    fun journalNeedsRecovery_mixedInnerPartitions() {
        openImage(fixture("nlmix.img"))
        unlockWithPassword("cui_nlmix")
        waitNoloadDialog()
        val ntfs = waitRoot("nlmix.img #2")   // 普通に開けた分は、確認を待たずに公開される
        assertEquals("volume seed 5042\n", readAll(ntfs, "/seed.txt").toString(Charsets.UTF_8))
        assertEquals(null, rootIdByTitle("nlmix.img #1"))
        assertEquals(1, VolumeRegistry.pendingCryptoList().size)
        clickText(s(R.string.noload_dialog_confirm))
        val ext4 = waitRoot("nlmix.img #1")
        assertEquals("volume seed 5041\n", readAll(ext4, "/seed.txt").toString(Charsets.UTF_8))
        assertEquals(0, rootFlags(ext4) and Root.FLAG_SUPPORTS_CREATE)
        assertTrue(VolumeRegistry.pendingCryptoList().isEmpty())
        clickTag("crypto_lock", 0)
        waitRootGone("nlmix.img #1")
        waitRootGone("nlmix.img #2")
        assertTrue(VolumeRegistry.cryptoEntries().isEmpty())
        assertNoSecretLeak()
    }

    // ---- パスワードの記憶 (SecretVault。docs/crypto-volumes.md「Android Keystore」)
    // emulator では実際の生体認証が使えないので、鍵は本物の AndroidKeyStore (認証なし = requireAuth=false)、認証 (BiometricPrompt の代わり) だけを差し替える。
    // BiometricPrompt の画面と「使うたびに認証」の鍵の挙動は、この試験では確かめられない (実機で確認)。

    private class TestAuth : UserAuth {
        @Volatile var mode = 0   // 0 = 認証 OK / 1 = 取り消し / 2 = 失敗
        @Volatile var calls = 0
        override fun authenticate(cipher: Cipher, title: String, subtitle: String): AuthResult {
            calls++
            return when (mode) { 0 -> AuthResult.Ok(cipher); 1 -> AuthResult.Cancelled; else -> AuthResult.Failed("test lockout") }
        }
    }

    /** 本物の Keystore 鍵。[invalidated] = true で、鍵が無効化されたとき (KeyPermanentlyInvalidatedException 相当) の復号の失敗を起こす。 */
    private class TestKeys(real: VaultKeys) : VaultKeys by real {
        private val inner = real
        @Volatile var invalidated = false
        override fun decryptCipher(alias: String, iv: ByteArray): Cipher {
            if (invalidated) throw KeyInvalidatedException("test: invalidated")
            return inner.decryptCipher(alias, iv)
        }
    }

    private val vaultPrefsName = "usbmem_vault_test"
    private lateinit var vAuth: TestAuth
    private lateinit var vKeys: TestKeys
    private lateinit var vStore: PrefsVaultStore

    private fun vaultAliases(): List<String> {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        return ks.aliases().toList().filter { it.startsWith("usbmem.vault.") }
    }
    private fun cleanVaultState() {
        runCatching {
            val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            ks.aliases().toList().filter { it.startsWith("usbmem.vault.") }.forEach { ks.deleteEntry(it) }
        }
        ctx.getSharedPreferences(vaultPrefsName, 0).edit().clear().commit()
    }
    private fun installVault() {
        cleanVaultState()
        vAuth = TestAuth()
        vKeys = TestKeys(AndroidKeystoreKeys(requireAuth = false))
        vStore = PrefsVaultStore(ctx, vaultPrefsName)
        VaultProvider.override = SecretVault(vKeys, vAuth, vStore)
    }
    private fun openUnlockDialog(index: Int = 0) { clickTag("crypto_unlock_button", index); waitTag("unlock_dialog") }
    private fun assertNoRememberOffered() {
        rule.onAllNodesWithTag("unlock_remember").assertCountEquals(0)
        rule.onAllNodesWithTag("unlock_remembered").assertCountEquals(0)
    }
    /** 「記憶する」にチェックを入れて、パスワードで解錠する (成功すると、保存のあとでダイアログが閉じる)。 */
    private fun unlockAndRemember(key: String) {
        openUnlockDialog()
        tag("unlock_remember").assertIsOff()    // 既定は記憶しない
        clickTag("unlock_remember")
        tag("unlock_remember").assertIsOn()
        typePasswordAndSubmit(pw.getValue(key))
        waitUnlocked()
    }

    /** (3) 誤ったパスワードでは保存されない → (1) 正しいパスワードで記憶 → 次回は入力なしで (記憶経由で) 解錠 → (2) 忘れると通常の入力に戻る。 */
    @Test
    fun vault_rememberUnlockForget() {
        installVault()
        openImage(fixture("whole.img"))

        // 既定は記憶しない: チェックは切。パスワードを入れても、チェックしなければ何も保存されない
        openUnlockDialog()
        tag("unlock_remember").assertIsOff()
        rule.onAllNodesWithTag("unlock_remembered").assertCountEquals(0)
        // (3) 誤ったパスワード + 記憶にチェック: 認証失敗で、何も保存されない (鍵も作られない)
        clickTag("unlock_remember")
        tag("unlock_password").performTextReplacement("definitely-wrong-password")
        tag("unlock_submit").assertIsEnabled().performClick()
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.unlock_err_password)).assertExists()
        assertTrue(vStore.slots().toString(), vStore.slots().isEmpty())
        assertTrue(vaultAliases().isEmpty())
        assertEquals(0, vAuth.calls)
        // チェックのまま正しいパスワード → 解錠でき、記憶が 1 件だけ保存される
        typePasswordAndSubmit(pw.getValue("cui_whole"))
        waitUnlocked()
        val a = waitRoot("whole.img #1")
        assertEquals("volume seed 5011\n", readAll(a, "/seed.txt").toString(Charsets.UTF_8))
        assertEquals(1, vStore.slots().size)
        assertEquals(1, vaultAliases().size)
        assertEquals("保存の認証は 1 回", 1, vAuth.calls)
        // 暗号文に平文のパスワードは含まれない
        val blob = vStore.get(vStore.slots().single())!!
        assertFalse(String(blob, Charsets.ISO_8859_1).contains(pw.getValue("cui_whole")))
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")

        // (1) 次回: 記憶済みの表示。記憶経由 (認証 → 取り出し) で、パスワードを入れずに解錠できる
        openUnlockDialog()
        waitTag("unlock_remembered")
        rule.onAllNodesWithTag("unlock_remember").assertCountEquals(0)    // 記憶済みなら、記憶のチェックは出さない
        clickTag("unlock_remembered")
        waitUnlocked()
        val b = waitRoot("whole.img #1")
        assertEquals("volume seed 5011\n", readAll(b, "/seed.txt").toString(Charsets.UTF_8))
        assertEquals("volume seed 5012\n", readAll(waitRoot("whole.img #2"), "/seed.txt").toString(Charsets.UTF_8))
        assertEquals("取り出しの認証は 1 回 (計 2 回)", 2, vAuth.calls)
        assertEquals(1, vStore.slots().size)    // 取り出しても記憶は残る
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")

        // (2) 忘れる: ダイアログの「記憶を削除」で、暗号文も鍵も消え、通常のパスワード入力 (と記憶のチェック) に戻る
        openUnlockDialog()
        waitTag("unlock_forget")
        clickTag("unlock_forget")
        // 確認なしで消えない: まず確認が出る。キャンセルすると、記憶はそのまま残る
        waitText(s(R.string.unlock_forget_confirm_text))
        clickTag("confirm_cancel")
        waitGone("dialog_body")
        assertEquals(1, vStore.slots().size)
        tag("unlock_remembered").assertExists()
        // もう一度押して「削除」で確定する
        clickTag("unlock_forget")
        waitText(s(R.string.unlock_forget_confirm_text))
        clickTag("confirm_ok")
        waitGone("unlock_remembered")
        waitTag("unlock_remember")
        tag("unlock_remember").assertIsOff()
        assertTrue(vStore.slots().isEmpty())
        assertTrue(vaultAliases().isEmpty())
        // 忘れたあとも、パスワードを入力すれば解錠できる (チェックしないので、また保存はされない)
        typePasswordAndSubmit(pw.getValue("cui_whole"))
        waitUnlocked()
        waitRoot("whole.img #1")
        assertTrue(vStore.slots().isEmpty())
        assertEquals(2, vAuth.calls)
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        assertNoSecretLeak()
    }

    /** 記憶が使えない端末 (画面ロックが無いなど) では、「記憶する」が黙って消えず、理由が一言出る。 */
    @Test
    fun vault_unsupportedDeviceShowsWhy() {
        installVault()
        VaultProvider.override = SecretVault(vKeys, vAuth, vStore, support = { VaultSupport.NoCredential })
        openImage(fixture("whole.img"))
        openUnlockDialog()
        assertNoRememberOffered()
        rule.onAllNodesWithTag("unlock_remember").assertCountEquals(0)
        tag("unlock_remember_unavailable").assertExists()
        rule.onNodeWithText(s(R.string.unlock_remember_unavailable_no_lock), substring = true).assertExists()
        // 「記憶する」を選ばない限り…という指す先の無い文も出さない
        rule.onAllNodesWithText(s(R.string.unlock_not_saved), substring = true).assertCountEquals(0)
        clickText(s(R.string.common_cancel))
        waitGone("unlock_dialog")
        VaultProvider.override = SecretVault(vKeys, vAuth, vStore, support = { VaultSupport.OsTooOld })
        openUnlockDialog()
        rule.onNodeWithText(s(R.string.unlock_remember_unavailable_os), substring = true).assertExists()
        clickText(s(R.string.common_cancel))
        waitGone("unlock_dialog")
    }

    /** 記憶経由の認証を取り消したら、ダイアログは開いたまま通常のパスワード入力に戻れる (記憶は残る)。保存のときの取り消しでは何も保存しないが、解錠は成功する。 */
    @Test
    fun vault_cancelFallsBackToPassword() {
        installVault()
        openImage(fixture("whole.img"))

        // 保存の認証を取り消す: 解錠は成功し、記憶は保存されない
        vAuth.mode = 1
        unlockAndRemember("cui_whole")
        waitRoot("whole.img #1")
        assertTrue(vStore.slots().isEmpty())
        assertTrue(vaultAliases().isEmpty())
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")

        // 保存できた状態にして、取り出しの認証を取り消す
        vAuth.mode = 0
        unlockAndRemember("cui_whole")
        waitRoot("whole.img #1")
        assertEquals(1, vStore.slots().size)
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")

        vAuth.mode = 1
        openUnlockDialog()
        clickTag("unlock_remembered")
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.vault_recall_cancelled)).assertExists()
        tag("unlock_dialog").assertExists()
        assertEquals(null, rootIdByTitle("whole.img #1"))
        assertEquals("取り消しでは記憶を捨てない", 1, vStore.slots().size)
        tag("unlock_remembered").assertExists()
        // 認証の失敗 (ロックアウトなど) も同じ: パスワード入力に戻れる
        vAuth.mode = 2
        clickTag("unlock_remembered")
        rule.waitUntil(30_000) { rule.onAllNodesWithText(ctx.getString(R.string.vault_recall_failed, "test lockout")).fetchSemanticsNodes().isNotEmpty() }
        assertEquals(1, vStore.slots().size)
        // パスワードを入れれば解錠できる
        typePasswordAndSubmit(pw.getValue("cui_whole"))
        waitUnlocked()
        waitRoot("whole.img #1")
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        assertNoSecretLeak()
    }

    /** (4) 鍵が無効化された (生体登録の変更など) / 鍵が消えた: 記憶を破棄して通常のパスワード入力にフォールバックする。 */
    @Test
    fun vault_invalidatedKeyDiscardsMemoryAndFallsBack() {
        installVault()
        openImage(fixture("whole.img"))

        // (a) KeyPermanentlyInvalidatedException 相当 (復号用 Cipher の用意で無効化が分かる)
        unlockAndRemember("cui_whole")
        waitRoot("whole.img #1")
        assertEquals(1, vStore.slots().size)
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        vKeys.invalidated = true
        openUnlockDialog()
        clickTag("unlock_remembered")
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.vault_recall_invalidated)).assertExists()
        waitGone("unlock_remembered")                      // 記憶済みの表示が消える
        assertTrue(vStore.slots().isEmpty())               // 暗号文は破棄済み
        assertTrue(vaultAliases().isEmpty())               // 鍵も消した
        waitTag("unlock_remember")                         // 通常の入力 (記憶のチェック) に戻る
        vKeys.invalidated = false
        typePasswordAndSubmit(pw.getValue("cui_whole"))    // パスワードで解錠できる (記憶なし)
        waitUnlocked()
        waitRoot("whole.img #1")
        assertTrue(vStore.slots().isEmpty())
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")

        // (b) Keystore の鍵そのものが消えた (別の端末への復元・画面ロックの解除): 暗号文だけが残っている状態も、同じく破棄してフォールバック
        unlockAndRemember("cui_whole")
        waitRoot("whole.img #1")
        assertEquals(1, vStore.slots().size)
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        vaultAliases().forEach { ks.deleteEntry(it) }
        assertTrue(vaultAliases().isEmpty())
        assertEquals(1, vStore.slots().size)
        openUnlockDialog()
        clickTag("unlock_remembered")
        waitTag("unlock_error")
        rule.onNodeWithText(s(R.string.vault_recall_invalidated)).assertExists()
        waitGone("unlock_remembered")
        assertTrue(vStore.slots().isEmpty())
        typePasswordAndSubmit(pw.getValue("cui_whole"))
        waitUnlocked()
        waitRoot("whole.img #1")
        clickTag("crypto_lock", 0)
        waitRootGone("whole.img #1")
        assertNoSecretLeak()
    }

    /** 記憶はボリュームごと: LUKS2 を記憶しても、同じイメージの BitLocker には出ない。BitLocker (ボリューム GUID) も記憶・解錠できる。 */
    @Test
    fun vault_perVolumeLuksAndBitlocker() {
        installVault()
        openImage(fixture("gpt.img"))
        waitTag("crypto_unlock_button", 2)

        unlockAndRemember("cui_gpt_luks")
        waitRoot("gpt.img #1")
        assertEquals(1, vStore.slots().size)
        clickTag("crypto_lock")
        waitRootGone("gpt.img #1")

        // 2 つ目 (BitLocker) には、記憶済みの表示は出ない (鍵は別)
        openUnlockDialog(1)
        rule.onAllNodesWithTag("unlock_remembered").assertCountEquals(0)
        tag("unlock_remember").assertIsOff()
        clickTag("unlock_remember")
        typePasswordAndSubmit(pw.getValue("cui_gpt_bl"))
        waitUnlocked()
        waitRoot("gpt.img #2")
        assertEquals("ボリュームごとに別の暗号文・別の鍵", 2, vStore.slots().size)
        assertEquals(2, vaultAliases().size)
        clickTag("crypto_lock")
        waitRootGone("gpt.img #2")

        // BitLocker を記憶経由で解錠
        vAuth.calls = 0
        openUnlockDialog(1)
        waitTag("unlock_remembered")
        clickTag("unlock_remembered")
        waitUnlocked()
        assertEquals("volume seed 5002\n", readAll(waitRoot("gpt.img #2"), "/seed.txt").toString(Charsets.UTF_8))
        assertEquals(1, vAuth.calls)
        clickTag("crypto_lock")
        waitRootGone("gpt.img #2")

        // LUKS2 も記憶経由で解錠できる
        openUnlockDialog(0)
        waitTag("unlock_remembered")
        clickTag("unlock_remembered")
        waitUnlocked()
        assertEquals("volume seed 5001\n", readAll(waitRoot("gpt.img #1"), "/seed.txt").toString(Charsets.UTF_8))
        clickTag("crypto_lock")
        waitRootGone("gpt.img #1")
        assertNoSecretLeak()
    }
}
