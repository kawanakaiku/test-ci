package org.usbmem.app

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
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
import org.junit.Rule
import org.junit.Test
import java.io.File
import java.io.FileInputStream
import java.security.MessageDigest

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
        // 解錠したまま残らないように (他の試験・次の試験へ持ち越さない)
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
}
