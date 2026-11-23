// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import com.hierynomus.mssmb2.SMB2MessageCommandCode
import com.hierynomus.mssmb2.SMBApiException
import com.hierynomus.protocol.transport.TransportException
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.net.ConnectException
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.TimeoutException

/** SMB の純 JVM 試験 (サーバー・Android に依存しない部分): 設定の検査と JSON、パスの検査、例外の分類、RemoteFs の既定のコピー (メモリ上の偽の実装)、取り消し、秘密の配列。 */
class SmbUnitTest {
    // ---- 接続の設定
    private fun conn(name: String = "NAS", host: String = "nas.example", port: Int = 445, share: String = "docs", domain: String = "", user: String = "u") =
        SmbConnection(id = "0123456789ab", name = name, host = host, port = port, share = share, domain = domain, user = user)

    @Test fun validConnectionPasses() { assertNull(conn().validate()) }

    @Test fun invalidFieldsAreRejected() {
        assertEquals(SmbInputError.Name, conn(name = " ").validate())
        assertEquals(SmbInputError.Name, conn(name = "x".repeat(65)).validate())
        assertEquals(SmbInputError.Host, conn(host = "").validate())
        assertEquals(SmbInputError.Host, conn(host = "a b").validate())
        assertEquals(SmbInputError.Host, conn(host = "a/b").validate())
        assertEquals(SmbInputError.Host, conn(host = "u@h").validate())
        assertEquals(SmbInputError.Port, conn(port = 0).validate())
        assertEquals(SmbInputError.Port, conn(port = 65536).validate())
        assertEquals(SmbInputError.Share, conn(share = "").validate())
        assertEquals(SmbInputError.Share, conn(share = "a\\b").validate())
        assertEquals(SmbInputError.Share, conn(share = "a/b").validate())
        assertEquals(SmbInputError.Domain, conn(domain = "A\\B").validate())
        assertEquals(SmbInputError.User, conn(user = "a\\b").validate())
        assertNull(conn(host = "192.0.2.10", port = 4445).validate())
        assertNull(conn(host = "fe80::1").validate())
    }

    @Test fun jsonRoundTripKeepsEverythingButNoPassword() {
        val c = conn(domain = "WG", port = 4445).copy(minVersion = SmbMinVersion.V311, requireEncryption = true, requireSigning = true)
        val j = c.toJson().toString()
        assertFalse(j.contains("password", ignoreCase = true))
        val back = SmbConnection.listFromJson(SmbConnection.listToJson(listOf(c, conn().copy(id = "ffffffffffff"))))
        assertEquals(2, back.size)
        assertEquals(c, back[0])
    }

    @Test fun brokenJsonIsDroppedNotFatal() {
        assertEquals(0, SmbConnection.listFromJson("not json").size)
        assertEquals(0, SmbConnection.listFromJson(null).size)
        // 検証に通らない要素 (ホストが空) は捨て、通る要素は残す
        val good = conn().toJson()
        val bad = conn().toJson().put("host", "")
        val s = org.json.JSONArray().put(bad).put(good).toString()
        assertEquals(1, SmbConnection.listFromJson(s).size)
    }

    @Test fun unknownVersionFallsBackToDefaultAndSmb1IsNotSelectable() {
        assertEquals(SmbMinVersion.V21, SmbMinVersion.of("1.0"))
        assertEquals(SmbMinVersion.V21, SmbMinVersion.of(null))
        assertTrue(SmbMinVersion.entries.none { it.key.startsWith("1") })
        assertEquals(SmbMinVersion.V311, SmbMinVersion.of("3.1.1"))
    }

    @Test fun targetShowsPortOnlyWhenNotDefault() {
        assertEquals("nas.example/docs", conn().target())
        assertEquals("nas.example:4445/docs", conn(port = 4445).target())
        assertEquals("[fe80::1]/docs", conn(host = "fe80::1").target())
    }

    @Test fun idsAreRandomHex() {
        val a = SmbConnection.newId(); val b = SmbConnection.newId()
        assertEquals(12, a.length)
        assertTrue(a.all { it in '0'..'9' || it in 'a'..'f' })
        assertFalse(a == b)
    }

    @Test fun emptyUserMeansGuest() { assertTrue(conn(user = "").isGuest); assertFalse(conn().isGuest) }

    // ---- パス
    @Test fun pathsMapToSmbBackslashes() {
        assertEquals("", SmbPaths.toSmb("/"))
        assertEquals("a", SmbPaths.toSmb("/a"))
        assertEquals("a\\b\\c.txt", SmbPaths.toSmb("/a/b//c.txt"))
        assertEquals("日本語\\ファイル.txt", SmbPaths.toSmb("/日本語/ファイル.txt"))
    }

    @Test fun pathTraversalAndBadNamesAreRejected() {
        for (p in listOf("/..", "/a/../b", "/a/./b", "/a\\b", "/a:b", "/a*", "/a?", "/a\"", "/a<", "/a>", "/a|", "/a\u0001")) {
            try { SmbPaths.toSmb(p); fail("accepted: $p") } catch (e: SmbException) { assertEquals(SmbErrorKind.InvalidName, e.kind) }
        }
        assertFalse(SmbPaths.validName(""))
        assertFalse(SmbPaths.validName("x".repeat(256)))
        assertTrue(SmbPaths.validName("x".repeat(255)))
    }

    @Test fun pathHelpers() {
        assertEquals("/", SmbPaths.parent("/a"))
        assertEquals("/a", SmbPaths.parent("/a/b"))
        assertEquals("b", SmbPaths.name("/a/b"))
        assertEquals("", SmbPaths.name("/"))
        assertEquals("/a", SmbPaths.child("/", "a"))
        assertEquals("/a/b", SmbPaths.child("/a", "b"))
    }

    // ---- 例外の分類
    @Test fun statusCodesMapToKinds() {
        assertEquals(SmbErrorKind.Auth, SmbErrors.kindOfStatus(0xC000006DL))
        assertEquals(SmbErrorKind.Auth, SmbErrors.kindOfStatus(0xC0000234L))
        assertEquals(SmbErrorKind.AccessDenied, SmbErrors.kindOfStatus(0xC0000022L))
        assertEquals(SmbErrorKind.NotFound, SmbErrors.kindOfStatus(0xC0000034L))
        assertEquals(SmbErrorKind.NotFound, SmbErrors.kindOfStatus(0xC000003AL))
        assertEquals(SmbErrorKind.ShareNotFound, SmbErrors.kindOfStatus(0xC00000CCL))
        assertEquals(SmbErrorKind.Exists, SmbErrors.kindOfStatus(0xC0000035L))
        assertEquals(SmbErrorKind.NotEmpty, SmbErrors.kindOfStatus(0xC0000101L))
        assertEquals(SmbErrorKind.NoSpace, SmbErrors.kindOfStatus(0xC000007FL))
        assertEquals(SmbErrorKind.Busy, SmbErrors.kindOfStatus(0xC0000043L))
        assertEquals(SmbErrorKind.Other, SmbErrors.kindOfStatus(0x12345L))
    }

    @Test fun apiExceptionMapsByStatus() {
        val e = SMBApiException(0xC000006DL, SMB2MessageCommandCode.SMB2_SESSION_SETUP, "Authentication failed for 'someuser' with password", null)
        val m = SmbErrors.map(e, connecting = true)
        assertEquals(SmbErrorKind.Auth, m.kind)
        // サーバー・SMBJ の文字列 (ユーザー名・パスワードの語を含む) をメッセージへ持ち込まない
        assertFalse(m.message!!.contains("someuser"))
        assertFalse(m.message!!.contains("password", ignoreCase = true))
        assertNull(m.cause)
    }

    @Test fun networkFailuresDependOnPhase() {
        assertEquals(SmbErrorKind.Network, SmbErrors.map(UnknownHostException("x"), connecting = true).kind)
        assertEquals(SmbErrorKind.Network, SmbErrors.map(ConnectException("x"), connecting = true).kind)
        assertEquals(SmbErrorKind.Network, SmbErrors.map(TransportException(SocketException("reset")), connecting = true).kind)
        assertEquals(SmbErrorKind.Disconnected, SmbErrors.map(TransportException(SocketException("reset"))).kind)
        assertEquals(SmbErrorKind.Disconnected, SmbErrors.map(java.io.EOFException()).kind)
        assertEquals(SmbErrorKind.Timeout, SmbErrors.map(TransportException(TimeoutException("t"))).kind)
        assertEquals(SmbErrorKind.Timeout, SmbErrors.map(SocketTimeoutException("t")).kind)
        assertEquals(SmbErrorKind.Cancelled, SmbErrors.map(SmbCancelled()).kind)
        assertEquals(SmbErrorKind.Cancelled, SmbErrors.map(InterruptedException()).kind)
        assertTrue(SmbErrors.isConnectionLost(SmbErrorKind.Timeout))
        assertFalse(SmbErrors.isConnectionLost(SmbErrorKind.NotFound))
    }

    @Test fun exceptionMessageIsFixedAndPathFree() {
        val m = SmbErrors.map(RuntimeException("secret-path/and-password=hunter2"))
        assertEquals(SmbErrorKind.Other, m.kind)
        assertFalse(m.message!!.contains("hunter2"))
        assertFalse(m.message!!.contains("secret-path"))
    }

    // ---- 取り消し
    @Test fun cancelFlagAndHook() {
        val c = SmbCancel()
        c.check()
        var hooked = 0
        c.onCancel { hooked++ }
        assertEquals(0, hooked)
        c.cancel()
        assertEquals(1, hooked)
        try { c.check(); fail() } catch (_: SmbCancelled) { }
        // 取り消し済みに後から登録したフックは、すぐ呼ばれる
        var late = 0
        c.onCancel { late++ }
        assertEquals(1, late)
    }

    // ---- 秘密の配列
    @Test fun secretsRoundTripAndWipe() {
        val chars = "pässwörd-日本語-🔑".toCharArray()
        val bytes = SmbSecrets.toBytes(chars)
        assertArrayEquals("pässwörd-日本語-🔑".toByteArray(Charsets.UTF_8), bytes)
        val back = SmbSecrets.toChars(bytes)
        assertArrayEquals(chars, back)
        SmbSecrets.wipe(bytes); SmbSecrets.wipe(back)
        assertTrue(bytes.all { it == 0.toByte() })
        assertTrue(back.all { it == '\u0000' })
    }

    // ---- RemoteFs の既定のコピー (全部をメモリに載せず、バッファ単位で流す)
    private class MemFs : RemoteFs {
        val files = HashMap<String, ByteArray>()
        var maxChunkSeen = 0
        override val isAlive = true
        override val dialect = "mem"
        override val encrypted = false
        override val signed = false
        override val serverAllowsWrite = true
        override fun stat(path: String): RemoteEntry = files[path]?.let { RemoteEntry(SmbPaths.name(path), false, it.size.toLong(), 0) } ?: throw SmbException(SmbErrorKind.NotFound)
        override fun list(path: String): List<RemoteEntry> = emptyList()
        override fun openRead(path: String): RemoteReader {
            val d = files[path] ?: throw SmbException(SmbErrorKind.NotFound)
            return object : RemoteReader {
                override val size = d.size.toLong()
                override fun read(offset: Long, buf: ByteArray, off: Int, len: Int): Int {
                    maxChunkSeen = maxOf(maxChunkSeen, len)
                    if (offset >= d.size) return 0
                    val n = minOf(len.toLong(), d.size - offset).toInt()
                    System.arraycopy(d, offset.toInt(), buf, off, n); return n
                }
                override fun close() {}
            }
        }
        override fun openWrite(path: String, create: Boolean, truncate: Boolean, readToo: Boolean): RemoteWriter {
            if (!files.containsKey(path) && !create) throw SmbException(SmbErrorKind.NotFound)
            if (truncate || !files.containsKey(path)) files[path] = ByteArray(0)
            return object : RemoteWriter {
                override fun write(offset: Long, buf: ByteArray, off: Int, len: Int) {
                    val cur = files[path]!!
                    val end = (offset + len).toInt()
                    val out = if (end > cur.size) cur.copyOf(end) else cur
                    System.arraycopy(buf, off, out, offset.toInt(), len)
                    files[path] = out
                }
                override fun setLength(length: Long) { files[path] = files[path]!!.copyOf(length.toInt()) }
                override fun flush() {}
                override fun close() {}
            }
        }
        override fun createFile(path: String) { files[path] = ByteArray(0) }
        override fun mkdir(path: String) {}
        override fun delete(path: String) { files.remove(path) }
        override fun rename(from: String, to: String) { files[to] = files.remove(from)!! }
        override fun close() {}
    }

    @Test fun defaultCopyStreamsInBuffers() {
        val fs = MemFs()
        val data = ByteArray(3 * RemoteFs.COPY_BUFFER + 12345) { (it * 31 + 7).toByte() }
        val progress = ArrayList<Long>()
        val n = fs.copyFrom("/x.bin", ByteArrayInputStream(data), progress = { done, _ -> progress.add(done) })
        assertEquals(data.size.toLong(), n)
        assertArrayEquals(data, fs.files["/x.bin"])
        assertTrue(progress.size >= 4)
        assertEquals(data.size.toLong(), progress.last())
        val out = ByteArrayOutputStream()
        assertEquals(data.size.toLong(), fs.copyTo("/x.bin", out))
        assertArrayEquals(data, out.toByteArray())
        assertTrue("1 回の読み取りがバッファより大きい", fs.maxChunkSeen <= RemoteFs.COPY_BUFFER)
    }

    @Test fun copyCanBeCancelledBetweenChunks() {
        val fs = MemFs()
        fs.files["/big"] = ByteArray(5 * RemoteFs.COPY_BUFFER)
        val cancel = SmbCancel()
        var chunks = 0
        try {
            fs.copyTo("/big", ByteArrayOutputStream(), cancel, { _, _ -> if (++chunks == 2) cancel.cancel() })
            fail("not cancelled")
        } catch (_: SmbCancelled) { }
        assertEquals(2, chunks)
        assertNotNull(fs.files["/big"])
    }

    @Test fun existsUsesStat() {
        val fs = MemFs()
        fs.files["/a"] = ByteArray(1)
        assertTrue(fs.exists("/a")); assertFalse(fs.exists("/b"))
    }
}
