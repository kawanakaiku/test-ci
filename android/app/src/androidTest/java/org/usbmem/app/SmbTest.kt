package org.usbmem.app

import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.usbmem.app.smb.RemoteFs
import org.usbmem.app.smb.SmbCancel
import org.usbmem.app.smb.SmbCancelled
import org.usbmem.app.smb.SmbConnection
import org.usbmem.app.smb.SmbController
import org.usbmem.app.smb.SmbErrorKind
import org.usbmem.app.smb.SmbException
import org.usbmem.app.smb.SmbMinVersion
import org.usbmem.app.smb.SmbMounts
import org.usbmem.app.smb.SmbRecalled
import org.usbmem.app.smb.SmbRemoteFs
import org.usbmem.app.smb.SmbStore
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.KeyStore
import java.security.MessageDigest
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import javax.crypto.Cipher
import kotlin.concurrent.thread

/**
 * SMB (SMBJ) の計装試験。CI の runner 上の Samba (tools/ci_android_tests.sh の run_smb。共有は tmpfs、ユーザー・パスワードは使い捨て) へ、エミュレータから 10.0.2.2 で接続する。docs/smb.md。
 *
 * 試験が「サーバーに書いた」ものは、`SMB_EXPECT <共有>|<パス>|<sha256>|<サイズ>` / `SMB_ABSENT <共有>|<パス>` として logcat へ出す。
 * run_smb がそれを、サーバー側の実ファイル (sha256sum) と smbclient で独立に照合する (書いたものが本当に入ったか・消したものが本当に無いか)。
 * 切断・応答なしは、試験の中の TCP 中継 ([TcpProxy]) で起こす (ホストを止めずに、決まったタイミングで RST / 無応答にできる)。
 */
class SmbTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext
    private val auth = "org.usbmem.app.documents"
    private val cr get() = ctx.contentResolver

    private val host = InstrumentationRegistry.getArguments().getString("smb_host") ?: "10.0.2.2"
    private val port = InstrumentationRegistry.getArguments().getString("smb_port")?.toIntOrNull() ?: 4445
    private val user = "smbtest"
    private val goodPw = "ci-only-Pw-7f3a9"

    private fun pw(s: String = goodPw) = s.toCharArray()
    private fun conn(share: String = "test", name: String = "CI NAS", host: String = this.host, port: Int = this.port, minVersion: SmbMinVersion = SmbMinVersion.V21,
                     encrypt: Boolean = false, user: String = this.user) =
        SmbConnection(id = SmbConnection.newId(), name = name, host = host, port = port, share = share, user = user, minVersion = minVersion, requireEncryption = encrypt)

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    private fun expect(share: String, path: String, sha: String, size: Long) { Log.i("SMB_EXPECT", "$share|$path|$sha|$size") }
    private fun absent(share: String, path: String) { Log.i("SMB_ABSENT", "$share|$path") }
    private fun metric(s: String) { Log.i("SMB_METRIC", s) }

    @Before fun setUp() { SmbMounts.removeAll(ctx); VaultProvider.override = null }
    @After fun tearDown() { SmbMounts.removeAll(ctx); VaultProvider.override = null }

    // ---- 決まったデータ (メモリに載せない)
    /** [seed] から決まるバイト列を [total] バイト流す。流した分を [md] へも入れる (書いた内容の SHA-256 を、メモリに溜めずに得る)。 */
    private class PatternStream(private val total: Long, seed: Long, val md: MessageDigest = MessageDigest.getInstance("SHA-256")) : InputStream() {
        private var pos = 0L
        private var s = seed or 1L
        override fun read(): Int = throw UnsupportedOperationException()
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (pos >= total) return -1
            val n = minOf(len.toLong(), total - pos).toInt()
            var i = 0
            while (i < n) {
                s = s xor (s shl 13); s = s xor (s ushr 7); s = s xor (s shl 17)
                var v = s; var k = 0
                while (k < 8 && i < n) { b[off + i] = v.toByte(); v = v ushr 8; i++; k++ }
            }
            md.update(b, off, n)
            pos += n
            return n
        }
    }

    private class DigestSink(val md: MessageDigest = MessageDigest.getInstance("SHA-256")) : OutputStream() {
        var count = 0L
        override fun write(b: Int) { md.update(b.toByte()); count++ }
        override fun write(b: ByteArray, off: Int, len: Int) { md.update(b, off, len); count += len }
    }

    private fun patternBytes(n: Int, seed: Long): ByteArray { val o = ByteArrayOutputStream(); PatternStream(n.toLong(), seed).copyTo(o); return o.toByteArray() }

    private fun readText(fs: RemoteFs, path: String): String = ByteArrayOutputStream().also { fs.copyTo(path, it) }.toString("UTF-8")

    // ---- TCP 中継 (切断・無応答を決まったタイミングで起こす)
    private class TcpProxy(private val targetHost: String, private val targetPort: Int, private val blackhole: Boolean = false) : AutoCloseable {
        private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        val port: Int get() = server.localPort
        @Volatile var stalled = false
        @Volatile private var running = true
        private val socks = CopyOnWriteArrayList<Socket>()
        init {
            thread(isDaemon = true, name = "smb-test-proxy") {
                while (running) {
                    val c = try { server.accept() } catch (_: Exception) { break }
                    socks.add(c)
                    if (blackhole) continue   // 受け付けるだけで、何も返さない
                    thread(isDaemon = true) {
                        try {
                            val u = Socket(targetHost, targetPort); socks.add(u)
                            thread(isDaemon = true) { pump(u, c) }
                            pump(c, u)
                        } catch (_: Exception) { try { c.close() } catch (_: Exception) { } }
                    }
                }
            }
        }
        private fun pump(from: Socket, to: Socket) {
            val buf = ByteArray(65536)
            try {
                val i = from.getInputStream(); val o = to.getOutputStream()
                while (running) {
                    val n = i.read(buf)
                    if (n < 0) break
                    while (stalled && running) Thread.sleep(20)   // 止めている間は、読んだ分も相手へ渡さない
                    o.write(buf, 0, n); o.flush()
                }
            } catch (_: Exception) { }
            finally { try { to.close() } catch (_: Exception) { }; try { from.close() } catch (_: Exception) { } }
        }
        /** 通っている接続を全部閉じる (クライアントには切断に見える) */
        fun dropAll() { socks.forEach { try { it.close() } catch (_: Exception) { } }; socks.clear() }
        override fun close() { running = false; stalled = false; dropAll(); try { server.close() } catch (_: Exception) { } }
    }

    private fun uri(id: String) = DocumentsContract.buildDocumentUri(auth, id)
    private fun names(parentId: String): Set<String> {
        val out = HashSet<String>()
        cr.query(DocumentsContract.buildChildDocumentsUri(auth, parentId), arrayOf(Document.COLUMN_DISPLAY_NAME), null, null, null)!!.use { while (it.moveToNext()) out += it.getString(0) }
        return out
    }
    private fun rootRow(rootId: String): Triple<String, Int, String>? {
        cr.query(DocumentsContract.buildRootsUri(auth), null, null, null, null)!!.use { c ->
            while (c.moveToNext()) if (c.getString(c.getColumnIndexOrThrow("root_id")) == rootId)
                return Triple(c.getString(c.getColumnIndexOrThrow("title")), c.getInt(c.getColumnIndexOrThrow("flags")), c.getString(c.getColumnIndexOrThrow("summary")) ?: "")
        }
        return null
    }

    // ---------------------------------------------------------------- 接続・認証

    @Test
    fun connectListAndInfo() {
        val c = conn()
        val r = SmbController.test(c, pw())
        metric("connect dialect=${r.dialect} encrypted=${r.encrypted} signed=${r.signed} writeOk=${r.serverAllowsWrite} rootEntries=${r.rootEntries}")
        assertTrue(r.dialect.startsWith("SMB "))
        assertTrue("seed ディレクトリが見える", r.rootEntries >= 1)
        assertTrue(r.serverAllowsWrite)
        val fs = SmbRemoteFs.connect(c, pw())
        try {
            assertTrue(fs.isAlive)
            val root = fs.list("/").associateBy { it.name }
            assertTrue(root.getValue("seed").isDir)
            val seed = fs.list("/seed").associateBy { it.name }
            assertEquals(setOf("hello.txt", "日本語.txt", "blob.bin", "blob.bin.sha256"), seed.keys)
            assertEquals(17L, seed.getValue("hello.txt").size)
            assertFalse(seed.getValue("hello.txt").isDir)
            assertTrue(seed.getValue("hello.txt").mtimeMs > 1_500_000_000_000L)
            assertEquals("hello from samba\n", readText(fs, "/seed/hello.txt"))
            assertEquals("こんにちは\n", readText(fs, "/seed/日本語.txt"))
            assertTrue(fs.stat("/").isDir)
            assertTrue(fs.stat("/seed").isDir)
            assertEquals(17L, fs.stat("/seed/hello.txt").size)
            try { fs.stat("/nope"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.NotFound, e.kind) }
            try { fs.list("/nope"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.NotFound, e.kind) }
            try { fs.stat("/seed/../x"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.InvalidName, e.kind) }
        } finally { fs.close() }
        assertFalse(fs.isAlive)
    }

    @Test
    fun wrongPasswordUnknownUserShareAndHostFail() {
        val t0 = System.nanoTime()
        val bad = "definitely-wrong-PASSWORD-123"
        try { SmbController.test(conn(), pw(bad)); fail("wrong password accepted") } catch (e: SmbException) {
            assertEquals(SmbErrorKind.Auth, e.kind)
            assertFalse("例外のメッセージにパスワードを含めない", (e.message ?: "").contains(bad))
            assertFalse((e.message ?: "").contains(user))
            assertNull(e.cause)
        }
        try { SmbController.test(conn(user = "no-such-user"), pw()); fail("unknown user accepted") } catch (e: SmbException) { assertEquals(SmbErrorKind.Auth, e.kind) }
        // ゲスト (ユーザー名なし): サーバーは map to guest = never。結果は情報として出す (Samba の版で、匿名の扱いが違うことがある)
        metric("guest: " + runCatching { SmbController.test(conn(user = ""), pw("")) }.fold({ "accepted" }, { (it as? SmbException)?.kind?.name ?: it.javaClass.simpleName }))
        try { SmbController.test(conn(share = "no-such-share"), pw()); fail("unknown share accepted") } catch (e: SmbException) { assertEquals(SmbErrorKind.ShareNotFound, e.kind) }
        try { SmbController.test(conn(port = 1), pw()); fail("closed port accepted") } catch (e: SmbException) { assertEquals(SmbErrorKind.Network, e.kind) }
        try { SmbController.test(conn(host = "no-such-host.invalid"), pw()); fail("unknown host accepted") } catch (e: SmbException) { assertEquals(SmbErrorKind.Network, e.kind) }
        metric("failures took ${(System.nanoTime() - t0) / 1_000_000} ms")
        // 失敗しても、渡したパスワードの配列はゼロになっている
        val arr = pw(bad)
        try { SmbRemoteFs.connect(conn(), arr); fail() } catch (_: SmbException) { }
        assertTrue(arr.all { it == '\u0000' })
        // 失敗のあとに、正しいパスワードで普通に接続できる (ロックアウトなどが起きていない)
        assertTrue(SmbController.test(conn(), pw()).rootEntries >= 1)
    }

    @Test
    fun dialectRangeAndSigning() {
        val a = SmbController.test(conn(minVersion = SmbMinVersion.V311), pw())
        assertEquals("SMB 3.1.1", a.dialect)
        val b = SmbController.test(conn(minVersion = SmbMinVersion.V202), pw())
        metric("negotiated with V202 range: ${b.dialect}")
        assertTrue(b.dialect.startsWith("SMB 3") || b.dialect.startsWith("SMB 2"))
        val c = SmbController.test(conn().copy(requireSigning = true), pw())
        metric("signing required by client: signed=${c.signed}")
        assertTrue(c.signed)
    }

    // ---------------------------------------------------------------- 読み取り・書き込み・削除・名前変更

    @Test
    fun readServerFileWithShaCheck() {
        val fs = SmbRemoteFs.connect(conn(), pw())
        try {
            val expected = readText(fs, "/seed/blob.bin.sha256").trim()
            val sink = DigestSink()
            val progress = ArrayList<Long>()
            val n = fs.copyTo("/seed/blob.bin", sink, progress = { d, _ -> progress.add(d) })
            assertEquals(6L * 1024 * 1024, n)
            assertEquals(expected, hex(sink.md.digest()))
            assertTrue("進捗が単調に増えて最後がサイズ", progress.isNotEmpty() && progress.zipWithNext().all { (a, b) -> b > a } && progress.last() == n)
            // ランダムアクセス (途中から・EOF 超え)
            fs.openRead("/seed/blob.bin").use { r ->
                assertEquals(n, r.size)
                val b = ByteArray(1000)
                assertEquals(1000, r.read(n - 1000, b, 0, 1000))
                assertEquals(0, r.read(n, b, 0, 1000))
                assertEquals(500, r.read(n - 500, b, 0, 1000))
            }
        } finally { fs.close() }
    }

    @Test
    fun writeOverwriteAppendAndVerifyOnServer() {
        val fs = SmbRemoteFs.connect(conn(), pw())
        try {
            if (!fs.exists("/out")) fs.mkdir("/out")
            // 新規作成 (3 MiB + 7 B) → 読み戻し → サーバー側の照合用に記録
            val md = MessageDigest.getInstance("SHA-256")
            val size = 3L * 1024 * 1024 + 7
            assertEquals(size, fs.copyFrom("/out/w.bin", PatternStream(size, 11, md)))
            val w = hex(md.digest())
            val sink = DigestSink(); fs.copyTo("/out/w.bin", sink)
            assertEquals(w, hex(sink.md.digest())); assertEquals(size, sink.count)
            assertEquals(size, fs.stat("/out/w.bin").size)
            expect("test", "out/w.bin", w, size)
            // 上書き (切り詰め) で小さくなる
            val small = "short text 日本語\n".toByteArray(Charsets.UTF_8)
            fs.copyFrom("/out/w.bin", small.inputStream())
            assertEquals(small.size.toLong(), fs.stat("/out/w.bin").size)
            assertEquals("short text 日本語\n", readText(fs, "/out/w.bin"))
            // 切り詰めずに途中へ書く / 末尾を伸ばす (位置指定の書き込み)
            fs.openWrite("/out/w.bin", create = false, truncate = false).use { it.write(6L, "TEXT".toByteArray(), 0, 4); it.flush() }
            fs.openWrite("/out/w.bin", create = false, truncate = false).use { it.write(small.size.toLong() + 5, "END".toByteArray(), 0, 3) }
            val after = ByteArrayOutputStream().also { fs.copyTo("/out/w.bin", it) }.toByteArray()
            assertEquals(small.size + 5 + 3, after.size)
            assertEquals("short TEXT", String(after, 0, 10, Charsets.UTF_8))
            assertEquals("END", String(after, after.size - 3, 3, Charsets.UTF_8))
            assertTrue(after.sliceArray(small.size until small.size + 5).all { it == 0.toByte() })   // 間は 0 で埋まる
            val d = MessageDigest.getInstance("SHA-256").digest(after)
            expect("test", "out/w.bin", hex(d), after.size.toLong())
            // 新規作成: 同名は Exists、開く (create=false) だけで作らない
            fs.createFile("/out/empty.txt")
            try { fs.createFile("/out/empty.txt"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.Exists, e.kind) }
            try { fs.openWrite("/out/ghost", create = false, truncate = false); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.NotFound, e.kind) }
            expect("test", "out/empty.txt", hex(MessageDigest.getInstance("SHA-256").digest(ByteArray(0))), 0)
            // 日本語と記号の名前
            val jp = "/out/日本語 ファイル (1) #&.txt"
            fs.copyFrom(jp, "あいうえお\n".toByteArray(Charsets.UTF_8).inputStream())
            assertTrue(fs.list("/out").any { it.name == "日本語 ファイル (1) #&.txt" })
            expect("test", "out/日本語 ファイル (1) #&.txt", hex(MessageDigest.getInstance("SHA-256").digest("あいうえお\n".toByteArray(Charsets.UTF_8))), "あいうえお\n".toByteArray(Charsets.UTF_8).size.toLong())
        } finally { fs.close() }
    }

    @Test
    fun renameMoveMkdirDelete() {
        val fs = SmbRemoteFs.connect(conn(), pw())
        try {
            if (!fs.exists("/ops")) fs.mkdir("/ops")
            fs.copyFrom("/ops/a.txt", "AAA".byteInputStream())
            // 名前変更
            fs.rename("/ops/a.txt", "/ops/b.txt")
            assertFalse(fs.exists("/ops/a.txt")); assertEquals("AAA", readText(fs, "/ops/b.txt"))
            // 同名があれば失敗し、どちらも壊れない
            fs.copyFrom("/ops/c.txt", "CCC".byteInputStream())
            try { fs.rename("/ops/b.txt", "/ops/c.txt"); fail("rename over existing") } catch (e: SmbException) { assertEquals(SmbErrorKind.Exists, e.kind) }
            assertEquals("AAA", readText(fs, "/ops/b.txt")); assertEquals("CCC", readText(fs, "/ops/c.txt"))
            // フォルダの作成と、ほかのフォルダへの移動 (名前も変える)
            fs.mkdir("/ops/sub")
            try { fs.mkdir("/ops/sub"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.Exists, e.kind) }
            fs.rename("/ops/b.txt", "/ops/sub/moved.txt")
            assertEquals("AAA", readText(fs, "/ops/sub/moved.txt"))
            assertEquals(setOf("sub", "c.txt"), fs.list("/ops").map { it.name }.toSet())
            // フォルダの名前変更 (中身ごと)
            fs.rename("/ops/sub", "/ops/sub2")
            assertEquals("AAA", readText(fs, "/ops/sub2/moved.txt"))
            // 中身のあるフォルダの削除 (再帰)
            fs.mkdir("/ops/tree"); fs.mkdir("/ops/tree/x"); fs.mkdir("/ops/tree/x/y")
            fs.copyFrom("/ops/tree/f1", "1".byteInputStream()); fs.copyFrom("/ops/tree/x/f2", "2".byteInputStream()); fs.copyFrom("/ops/tree/x/y/f3", "3".byteInputStream())
            fs.delete("/ops/tree")
            assertFalse(fs.exists("/ops/tree")); absent("test", "ops/tree"); absent("test", "ops/tree/x/y/f3")
            // ファイルの削除、無いものの削除は NotFound
            fs.delete("/ops/c.txt"); absent("test", "ops/c.txt")
            try { fs.delete("/ops/c.txt"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.NotFound, e.kind) }
            // ルートは消せない / 動かせない
            try { fs.delete("/"); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.AccessDenied, e.kind) }
            absent("test", "ops/a.txt"); absent("test", "ops/b.txt")
            // 残るもの (サーバー側で照合)
            expect("test", "ops/sub2/moved.txt", hex(MessageDigest.getInstance("SHA-256").digest("AAA".toByteArray())), 3)
        } finally { fs.close() }
    }

    @Test
    fun largeFileStreamsWithoutLoadingInMemory() {
        val total = 320L * 1024 * 1024
        val maxHeap = Runtime.getRuntime().maxMemory()
        metric("large: total=${total shr 20} MiB, heap limit=${maxHeap shr 20} MiB")
        // 基準: 通信なしで、データの生成と SHA-256 だけにかかる速さ (転送の遅さが、通信・署名・この生成のどれによるかを切り分ける)
        run {
            val base = DigestSink(); val t = System.nanoTime()
            PatternStream(64L * 1024 * 1024, 3).copyTo(base, 1 shl 20)
            metric("large baseline: generate+sha256 only ${64 * 1000 / maxOf(1, (System.nanoTime() - t) / 1_000_000)} MiB/s")
        }
        val fs = SmbRemoteFs.connect(conn(), pw())
        try {
            metric("large: negotiated ${fs.dialect}, signed=${fs.signed}, encrypted=${fs.encrypted}")
            if (!fs.exists("/out")) fs.mkdir("/out")
            val md = MessageDigest.getInstance("SHA-256")
            var peak = 0L
            val t0 = System.nanoTime()
            val written = fs.copyFrom("/out/large.bin", PatternStream(total, 99, md), progress = { _, _ ->
                val used = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory(); if (used > peak) peak = used
            })
            val t1 = System.nanoTime()
            assertEquals(total, written)
            val want = hex(md.digest())
            assertEquals(total, fs.stat("/out/large.bin").size)
            val sink = DigestSink()
            val n = fs.copyTo("/out/large.bin", sink, progress = { _, _ ->
                val used = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory(); if (used > peak) peak = used
            })
            val t2 = System.nanoTime()
            assertEquals(total, n)
            assertEquals(want, hex(sink.md.digest()))
            metric("large: write ${(total shr 20) * 1000 / maxOf(1, (t1 - t0) / 1_000_000)} MiB/s, read ${(total shr 20) * 1000 / maxOf(1, (t2 - t1) / 1_000_000)} MiB/s, peak java heap ${peak shr 20} MiB")
            assertTrue("ヒープの使用が全体のサイズに比例していない (ストリーム)", peak < total / 2 || peak < maxHeap)
            assertTrue("320 MiB をメモリに載せていない: ピーク ${peak shr 20} MiB", peak < 200L * 1024 * 1024)
            expect("test", "out/large.bin", want, total)
        } finally { fs.close() }
    }

    // ---------------------------------------------------------------- 暗号化

    @Test
    fun encryptedShareRequiredEncryption() {
        val c = conn(share = "enc", encrypt = true)
        val fs = SmbRemoteFs.connect(c, pw())
        try {
            assertTrue("SMB 3 の暗号化が有効", fs.encrypted)
            assertTrue(fs.dialect.startsWith("SMB 3"))
            metric("encrypted share: dialect=${fs.dialect} encrypted=${fs.encrypted} signed=${fs.signed}")
            val md = MessageDigest.getInstance("SHA-256")
            val size = 4L * 1024 * 1024 + 3
            fs.copyFrom("/enc.bin", PatternStream(size, 5, md))
            val sink = DigestSink(); fs.copyTo("/enc.bin", sink)
            val wantEnc = hex(md.digest()); val gotEnc = hex(sink.md.digest())   // digest() は 1 回だけ (2 回目は空データのハッシュになる)
            assertEquals(wantEnc, gotEnc)
            expect("enc", "enc.bin", gotEnc, size)
            assertEquals("encrypted hello\n", readText(fs, "/hello-enc.txt"))
        } finally { fs.close() }
        // 暗号化を求めない接続での結果は、情報として出す (サーバーが共有ごとの暗号化を強制する)
        val r = runCatching { SmbController.test(conn(share = "enc"), pw()) }
        metric("enc share without client encryption: " + r.fold({ "ok (encrypted=${it.encrypted})" }, { (it as? SmbException)?.kind?.name ?: it.javaClass.simpleName }))
    }

    // ---------------------------------------------------------------- SAF (ドキュメント UI と同じ ContentResolver 経路)

    @Test
    fun safProviderListReadWriteRenameMoveDelete() {
        val c = conn(name = "SAF NAS")
        val m = SmbController.mount(ctx, c, pw(), writable = true)
        val root = m.id + ":/"
        try {
            assertTrue(m.writable)
            val row = rootRow(m.id)
            assertNotNull("ルートが出る", row)
            assertEquals("SAF NAS (SMB)", row!!.first)
            assertTrue(row.second and DocumentsContract.Root.FLAG_SUPPORTS_CREATE != 0)
            assertTrue("説明にバージョンと接続先", row.third.contains("SMB") && row.third.contains("test"))
            val top = names(root)
            assertTrue(top.contains("seed"))
            assertEquals(setOf("hello.txt", "日本語.txt", "blob.bin", "blob.bin.sha256"), names(m.id + ":/seed"))
            // 読み取り (小さい / 大きい + SHA-256)
            assertEquals("hello from samba\n", cr.openInputStream(uri(m.id + ":/seed/hello.txt"))!!.use { String(it.readBytes(), Charsets.UTF_8) })
            val expected = cr.openInputStream(uri(m.id + ":/seed/blob.bin.sha256"))!!.use { String(it.readBytes(), Charsets.UTF_8) }.trim()
            val sink = DigestSink()
            cr.openInputStream(uri(m.id + ":/seed/blob.bin"))!!.use { it.copyTo(sink, 256 * 1024) }
            assertEquals(6L * 1024 * 1024, sink.count)
            assertEquals(expected, hex(sink.md.digest()))
            cr.query(uri(m.id + ":/seed/hello.txt"), arrayOf(Document.COLUMN_SIZE, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS, Document.COLUMN_DISPLAY_NAME), null, null, null)!!.use { q ->
                assertTrue(q.moveToFirst()); assertEquals(17L, q.getLong(0)); assertEquals("text/plain", q.getString(1)); assertEquals("hello.txt", q.getString(3))
                assertTrue(q.getInt(2) and Document.FLAG_SUPPORTS_WRITE != 0); assertTrue(q.getInt(2) and Document.FLAG_SUPPORTS_DELETE != 0)
            }
            // フォルダ作成 → ファイル作成 → 書き込み → 読み戻し
            val dir = DocumentsContract.createDocument(cr, uri(root), Document.MIME_TYPE_DIR, "saf")!!
            val dirId = DocumentsContract.getDocumentId(dir)
            assertEquals(m.id + ":/saf", dirId)
            val f = DocumentsContract.createDocument(cr, dir, "application/octet-stream", "w.bin")!!
            val md = MessageDigest.getInstance("SHA-256")
            val size = 5L * 1024 * 1024 + 123
            cr.openOutputStream(f, "wt")!!.use { out -> PatternStream(size, 21, md).copyTo(out, 128 * 1024) }
            val want = hex(md.digest())
            val rs = DigestSink()
            cr.openInputStream(f)!!.use { it.copyTo(rs, 64 * 1024) }
            assertEquals(size, rs.count); assertEquals(want, hex(rs.md.digest()))
            assertEquals(size, m.fs.stat("/saf/w.bin").size)
            // 同名の作成は " (1)" を付ける (DocumentsProvider の約束)
            val dup = DocumentsContract.createDocument(cr, dir, "application/octet-stream", "w.bin")!!
            assertEquals("w (1).bin", DocumentsContract.getDocumentId(dup).substringAfterLast('/'))
            // 名前変更・移動・削除
            val renamed = DocumentsContract.renameDocument(cr, f, "w2.bin")!!
            assertEquals(m.id + ":/saf/w2.bin", DocumentsContract.getDocumentId(renamed))
            assertFalse(m.fs.exists("/saf/w.bin")); assertTrue(m.fs.exists("/saf/w2.bin"))
            val dir2 = DocumentsContract.createDocument(cr, uri(root), Document.MIME_TYPE_DIR, "saf_done")!!
            val moved = DocumentsContract.moveDocument(cr, renamed, dir, dir2)!!
            assertEquals(m.id + ":/saf_done/w2.bin", DocumentsContract.getDocumentId(moved))
            assertFalse(m.fs.exists("/saf/w2.bin"))
            assertTrue(DocumentsContract.deleteDocument(cr, dup))
            assertFalse(m.fs.exists("/saf/w (1).bin"))
            assertTrue(DocumentsContract.deleteDocument(cr, dir))   // 空になった saf フォルダ
            assertFalse(m.fs.exists("/saf"))
            assertTrue(names(m.id + ":/saf_done").contains("w2.bin"))
            expect("test", "saf_done/w2.bin", want, size)
            absent("test", "saf/w.bin"); absent("test", "saf")
            assertTrue(DocumentsContract.isChildDocument(cr, uri(root), uri(m.id + ":/saf_done/w2.bin")))
            assertFalse(DocumentsContract.isChildDocument(cr, uri(m.id + ":/saf_done"), uri(m.id + ":/seed/hello.txt")))
            // ".." は拒否 / 知らない共有 ID は失敗
            try { cr.query(uri(m.id + ":/seed/../out"), null, null, null, null)?.use { assertEquals(0, it.count) } } catch (_: Exception) { }
            try { cr.openInputStream(uri("smb-ffffffffffff:/x"))?.close(); fail() } catch (_: Exception) { }
            // ルートは消せない
            try { DocumentsContract.deleteDocument(cr, uri(root)); fail() } catch (_: Exception) { }
        } finally { SmbController.unmount(ctx, c) }
        assertNull("マウント解除でルートが消える", rootRow(m.id))
    }

    @Test
    fun safReadOnlyMountRefusesWrites() {
        val c = conn(name = "RO NAS")
        val m = SmbController.mount(ctx, c, pw(), writable = false)
        val root = m.id + ":/"
        try {
            assertFalse(m.writable)
            val row = rootRow(m.id)!!
            assertEquals(0, row.second and DocumentsContract.Root.FLAG_SUPPORTS_CREATE)
            cr.query(uri(m.id + ":/seed/hello.txt"), arrayOf(Document.COLUMN_FLAGS), null, null, null)!!.use { q -> assertTrue(q.moveToFirst()); assertEquals(0, q.getInt(0)) }
            assertEquals("hello from samba\n", cr.openInputStream(uri(m.id + ":/seed/hello.txt"))!!.use { String(it.readBytes(), Charsets.UTF_8) })
            try { DocumentsContract.createDocument(cr, uri(root), "text/plain", "nope.txt"); fail("create on read-only") } catch (_: Exception) { }
            for (mode in listOf("w", "wt", "rw")) try { cr.openFileDescriptor(uri(m.id + ":/seed/hello.txt"), mode)?.close(); fail("open $mode on read-only") } catch (_: Exception) { }
            try { DocumentsContract.deleteDocument(cr, uri(m.id + ":/seed/hello.txt")); fail("delete on read-only") } catch (_: Exception) { }
            try { DocumentsContract.renameDocument(cr, uri(m.id + ":/seed/hello.txt"), "x.txt"); fail("rename on read-only") } catch (_: Exception) { }
            assertEquals("hello from samba\n", readText(m.fs, "/seed/hello.txt"))
        } finally { SmbController.unmount(ctx, c) }
    }

    @Test
    fun usbRootsAndSmbRootsCoexist() {
        // USB のボリューム (ここでは使わない) の ID 形式と SMB の ID は混ざらない。SMB のルートだけが出て、他のルートの問い合わせも壊れない
        val c = conn(name = "Coexist")
        val m = SmbController.mount(ctx, c, pw(), writable = false)
        try {
            assertTrue(m.id.startsWith("smb-"))
            val before = cr.query(DocumentsContract.buildRootsUri(auth), null, null, null, null)!!.use { it.count }
            assertTrue(before >= 1)
            // USB 側の形式の ID (SMB の ID ではない) を問い合わせても、SMB のドキュメントは混ざらない (失敗するか、SMB 以外の行だけ)
            val foreign = ArrayList<String>()
            try {
                cr.query(DocumentsContract.buildChildDocumentsUri(auth, "0123456789ab:/"), arrayOf(Document.COLUMN_DOCUMENT_ID), null, null, null)?.use { while (it.moveToNext()) foreign += it.getString(0) }
            } catch (_: Exception) { }
            assertTrue("USB の ID の問い合わせに SMB の行が混ざった: $foreign", foreign.none { it.startsWith("smb-") })
            assertNotNull("マウントした SMB のルートが消えた", rootRow(m.id))
        } finally { SmbController.unmount(ctx, c) }
    }

    // ---------------------------------------------------------------- 切断・無応答・取り消し

    @Test
    fun disconnectDuringBigWriteEndsCleanly() {
        val proxy = TcpProxy(host, port)
        try {
            val fs = SmbRemoteFs.connect(conn(host = "127.0.0.1", port = proxy.port), pw(), timeoutSec = 10)
            if (!fs.exists("/out")) fs.mkdir("/out")
            val written = AtomicLong(); val err = AtomicReference<Throwable?>(null); val done = CountDownLatch(1)
            val t0 = System.nanoTime()
            thread(name = "smb-test-bigwrite") {
                try { fs.copyFrom("/out/drop.bin", PatternStream(512L * 1024 * 1024, 7), progress = { d, _ -> written.set(d) }) } catch (e: Throwable) { err.set(e) } finally { done.countDown() }
            }
            var waited = 0
            while (written.get() < 4L * 1024 * 1024 && waited < 600) { Thread.sleep(50); waited++ }
            assertTrue("書き込みが始まった", written.get() >= 4L * 1024 * 1024)
            proxy.dropAll()
            assertTrue("切断のあと 20 秒以内に転送が終わる (固まらない)", done.await(20, TimeUnit.SECONDS))
            val e = err.get()
            assertTrue("SmbException で終わる: $e", e is SmbException)
            val k = (e as SmbException).kind
            assertTrue("切断 / 応答なしの種類: $k", k == SmbErrorKind.Disconnected || k == SmbErrorKind.Timeout || k == SmbErrorKind.Network)
            metric("disconnect during write: ${k.name} after ${(System.nanoTime() - t0) / 1_000_000} ms, written=${written.get() shr 20} MiB")
            assertFalse(fs.isAlive)
            // 以後の操作はすぐ失敗する (また待たされない)
            val t1 = System.nanoTime()
            try { fs.list("/"); fail() } catch (x: SmbException) { assertEquals(SmbErrorKind.Disconnected, x.kind) }
            assertTrue((System.nanoTime() - t1) / 1_000_000 < 2000)
            fs.close()   // 切れた接続の close も固まらない
        } finally { proxy.close() }
        // 途中まで書いたファイルは、サーバーに残っている。新しい接続で消す
        val fs2 = SmbRemoteFs.connect(conn(), pw())
        try { if (fs2.exists("/out/drop.bin")) fs2.delete("/out/drop.bin"); absent("test", "out/drop.bin") } finally { fs2.close() }
    }

    @Test
    fun unresponsiveServerTimesOutAndMountIsReaped() {
        val proxy = TcpProxy(host, port)
        try {
            val c = conn(host = "127.0.0.1", port = proxy.port, name = "Flaky")
            val m = SmbController.mount(ctx, c, pw(), writable = true, timeoutSec = 3)
            assertNotNull(rootRow(m.id))
            assertTrue(m.fs.list("/").isNotEmpty())
            proxy.stalled = true   // 以後、サーバーの応答が届かない
            val t0 = System.nanoTime()
            try {
                val got = m.fs.list("/")
                fail("stalled list returned ${got.size} entries after ${(System.nanoTime() - t0) / 1_000_000} ms (キャッシュから返った疑い)")
            } catch (e: SmbException) {
                assertEquals("kind=${e.kind}", SmbErrorKind.Timeout, e.kind)
            }
            val ms = (System.nanoTime() - t0) / 1_000_000
            metric("stalled request timed out after $ms ms (limit 3 s)")
            assertTrue("3 秒の上限を大きく超えない: $ms ms", ms < 15_000)
            assertFalse(m.fs.isAlive)
            // 切れたマウントは外れ、SAF のルートが消える。通知は一度だけ
            assertEquals(1, SmbMounts.reapDead(ctx))
            assertNull(rootRow(m.id))
            assertEquals(SmbErrorKind.Disconnected, SmbMounts.takeLost(c.id))
            assertNull(SmbMounts.takeLost(c.id))
            // 外れた共有のドキュメントは、固まらずに失敗する
            val t1 = System.nanoTime()
            try { cr.query(DocumentsContract.buildChildDocumentsUri(auth, m.id + ":/"), null, null, null, null)?.close(); fail() } catch (_: Exception) { }
            assertTrue((System.nanoTime() - t1) / 1_000_000 < 3000)
        } finally { proxy.close() }
    }

    @Test
    fun serverDropMarksMountDeadForSaf() {
        val proxy = TcpProxy(host, port)
        try {
            val c = conn(host = "127.0.0.1", port = proxy.port, name = "Dropper")
            val m = SmbController.mount(ctx, c, pw(), writable = false)
            assertEquals("hello from samba\n", cr.openInputStream(uri(m.id + ":/seed/hello.txt"))!!.use { String(it.readBytes(), Charsets.UTF_8) })
            proxy.dropAll()
            var i = 0
            while (m.fs.isAlive && i < 200) { Thread.sleep(50); i++ }
            assertFalse("切断を検出する", m.fs.isAlive)
            // SAF の読み取りは errno で失敗する (ハングしない)
            val t0 = System.nanoTime()
            try { cr.openInputStream(uri(m.id + ":/seed/hello.txt"))?.use { it.readBytes() }; fail("read after drop") } catch (_: Exception) { }
            assertTrue((System.nanoTime() - t0) / 1_000_000 < 5000)
            assertNull("ルート一覧からも消える", rootRow(m.id))
            assertTrue(SmbMounts.all().isEmpty())
        } finally { proxy.close() }
    }

    @Test
    fun cancelDuringConnectAndTransfer() {
        // 受け付けるだけで返事をしない相手への接続を、取り消せる
        TcpProxy(host, port, blackhole = true).use { black ->
            val cancel = SmbCancel()
            val res = AtomicReference<Throwable?>(null); val done = CountDownLatch(1)
            val t0 = System.nanoTime()
            thread { try { SmbRemoteFs.connect(conn(host = "127.0.0.1", port = black.port), pw(), cancel, timeoutSec = 30); res.set(AssertionError("connected")) } catch (e: Throwable) { res.set(e) } finally { done.countDown() } }
            Thread.sleep(1000)
            cancel.cancel()
            assertTrue("取り消しから 10 秒以内に戻る", done.await(10, TimeUnit.SECONDS))
            val e = res.get()
            assertTrue("取り消しの SmbException: $e", e is SmbException && e.kind == SmbErrorKind.Cancelled)
            metric("connect cancelled after ${(System.nanoTime() - t0) / 1_000_000} ms")
        }
        // 転送の取り消し (途中で止まり、接続は使い続けられる)
        val fs = SmbRemoteFs.connect(conn(), pw())
        try {
            val cancel = SmbCancel()
            var chunks = 0
            try { fs.copyTo("/seed/blob.bin", DigestSink(), cancel) { _, _ -> if (++chunks == 2) cancel.cancel() }; fail("not cancelled") } catch (_: SmbCancelled) { }
            assertEquals(2, chunks)
            assertTrue(fs.isAlive)
            assertEquals("hello from samba\n", readText(fs, "/seed/hello.txt"))
        } finally { fs.close() }
        // 取り消し済みの旗では、接続しない
        val c2 = SmbCancel().also { it.cancel() }
        try { SmbRemoteFs.connect(conn(), pw(), c2); fail() } catch (e: SmbException) { assertEquals(SmbErrorKind.Cancelled, e.kind) }
    }

    // ---------------------------------------------------------------- パスワードの扱い

    private class TestAuth : UserAuth {
        @Volatile var mode = 0   // 0 = 認証 OK / 1 = 取り消し
        override fun authenticate(cipher: Cipher, title: String, subtitle: String): AuthResult = if (mode == 0) AuthResult.Ok(cipher) else AuthResult.Cancelled
    }

    @Test
    fun passwordIsNotStoredByDefaultAndSavedOnlyEncrypted() {
        val vaultPrefs = "usbmem_vault_test_smb"
        runCatching {
            val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            ks.aliases().toList().filter { it.startsWith("usbmem.vault.") }.forEach { ks.deleteEntry(it) }
        }
        ctx.getSharedPreferences(vaultPrefs, 0).edit().clear().commit()
        val auth = TestAuth()
        VaultProvider.override = SecretVault(AndroidKeystoreKeys(requireAuth = false), auth, PrefsVaultStore(ctx, vaultPrefs))
        val c = conn(name = "PwTest")
        val secret = "Sekret-P@ss-日本語-4821"
        // 既定: 接続の保存だけでは、パスワードはどこにも残らない
        SmbStore(ctx).save(c)
        assertFalse(SmbController.hasSavedPassword(ctx, c))
        assertTrue(ctx.getSharedPreferences(vaultPrefs, 0).all.isEmpty())
        // 保存を選んだとき: Keystore で暗号化して置く。平文 (UTF-8 / Base64 / 文字列) が、どの保存先にも無い
        assertEquals(RememberResult.Saved, SmbController.rememberPassword(ctx, c, secret.toCharArray()))
        assertTrue(SmbController.hasSavedPassword(ctx, c))
        val all = ctx.getSharedPreferences(vaultPrefs, 0).all
        assertEquals(1, all.size)
        val blob = android.util.Base64.decode(all.values.first() as String, android.util.Base64.NO_WRAP)
        assertFalse(String(blob, Charsets.ISO_8859_1).contains("Sekret"))
        assertFalse(all.values.joinToString().contains("Sekret"))
        for (name in listOf("usbmem_smb", vaultPrefs)) assertFalse(ctx.getSharedPreferences(name, 0).all.toString().contains("Sekret"))
        for (f in java.io.File(ctx.applicationInfo.dataDir, "shared_prefs").listFiles().orEmpty()) assertFalse("${f.name} に平文", f.readText().contains("Sekret"))
        // 取り出し (認証が通る): 同じ文字列。取り消したら取り出せない
        val r = SmbController.recallPassword(ctx, c)
        assertTrue(r is SmbRecalled.Password)
        assertArrayEquals(secret.toCharArray(), (r as SmbRecalled.Password).chars)
        auth.mode = 1
        assertTrue(SmbController.recallPassword(ctx, c) is SmbRecalled.Cancelled)
        auth.mode = 0
        // 保存したパスワードで、実際に接続できる (取り出した配列は接続でゼロになる)
        SmbController.forgetPassword(ctx, c)
        assertFalse(SmbController.hasSavedPassword(ctx, c))
        assertTrue(SmbController.recallPassword(ctx, c) is SmbRecalled.NotSaved)
        assertEquals(RememberResult.Saved, SmbController.rememberPassword(ctx, c, pw()))
        val rec = (SmbController.recallPassword(ctx, c) as SmbRecalled.Password).chars
        val m = SmbController.mount(ctx, c, rec, writable = false)
        assertTrue(rec.all { it == '\u0000' })
        assertTrue(m.fs.isAlive)
        // 接続を消すと、保存したパスワードも、マウントも消える
        SmbController.delete(ctx, c)
        assertFalse(SmbController.hasSavedPassword(ctx, c))
        assertNull(SmbMounts.byConnection(c.id))
        assertTrue(SmbStore(ctx).all().none { it.id == c.id })
        assertTrue(ctx.getSharedPreferences(vaultPrefs, 0).all.isEmpty())
    }

    @Test
    fun connectionListPersistsWithoutPasswordAndSurvivesBrokenData() {
        val s = SmbStore(ctx)
        val before = s.all()
        val a = conn(name = "List A"); val b = conn(name = "List B", port = 4446)
        s.save(a); s.save(b); s.save(a.copy(name = "List A2"))
        val l = s.all()
        assertEquals(before.size + 2, l.size)
        assertEquals("List A2", l.first { it.id == a.id }.name)
        assertEquals(4446, l.first { it.id == b.id }.port)
        s.delete(a.id); s.delete(b.id)
        assertEquals(before.size, s.all().size)
        ctx.getSharedPreferences("usbmem_smb", 0).edit().putString("connections", "{broken").commit()
        assertTrue(SmbStore(ctx).all().isEmpty())
        ctx.getSharedPreferences("usbmem_smb", 0).edit().clear().commit()
    }
}
