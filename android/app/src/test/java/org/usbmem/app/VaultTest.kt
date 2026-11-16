package org.usbmem.app

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** [SecretVault] の純 JVM 試験: ソフトの AES-GCM 鍵と偽の認証で、暗号文の形式・保存・取り出し・破棄の判断を確かめる (Keystore / 生体認証そのものは計装テストと実機)。 */
class VaultTest {
    private class SoftKeys : VaultKeys {
        val keys = HashMap<String, SecretKey>()
        var invalidated = false
        override fun encryptCipher(alias: String): Cipher {
            val k = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
            keys[alias] = k
            return Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, k) }
        }
        override fun decryptCipher(alias: String, iv: ByteArray): Cipher {
            if (invalidated) throw KeyInvalidatedException("invalidated")
            val k = keys[alias] ?: throw KeyInvalidatedException("missing")
            return Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.DECRYPT_MODE, k, GCMParameterSpec(128, iv)) }
        }
        override fun delete(alias: String) { keys.remove(alias) }
    }

    private class MemStore : VaultStore {
        val m = HashMap<String, ByteArray>()
        override fun get(slot: String) = m[slot]?.copyOf()
        override fun put(slot: String, blob: ByteArray) { m[slot] = blob.copyOf() }
        override fun remove(slot: String) { m.remove(slot) }
        override fun slots(): Set<String> = m.keys.toSet()
    }

    private class FakeAuth : UserAuth {
        var mode = 0   // 0 = OK / 1 = 取り消し / 2 = 失敗
        var calls = 0
        override fun authenticate(cipher: Cipher, title: String, subtitle: String): AuthResult {
            calls++
            return when (mode) { 0 -> AuthResult.Ok(cipher); 1 -> AuthResult.Cancelled; else -> AuthResult.Failed("lockout") }
        }
    }

    private val keys = SoftKeys()
    private val store = MemStore()
    private val auth = FakeAuth()
    private val vault = SecretVault(keys, auth, store)
    private val pw = "correct horse battery staple".toByteArray()

    @Test fun rememberThenRecallRoundTrips() {
        assertFalse(vault.has("LUKS:u1"))
        assertEquals(RememberResult.Saved, vault.remember("LUKS:u1", pw, "t", "s"))
        assertTrue(vault.has("LUKS:u1"))
        val r = vault.recall("LUKS:u1", "t", "s") as RecallResult.Secret
        assertArrayEquals(pw, r.bytes)
        assertEquals(2, auth.calls)   // 保存と取り出しで 1 回ずつ認証
    }

    @Test fun storedBlobHasNoPlaintextAndNoRawId() {
        vault.remember("LUKS:u1", pw, "t", "s")
        val (slot, blob) = store.m.entries.single().toPair()
        assertFalse(String(blob, Charsets.ISO_8859_1).contains(String(pw)))
        assertFalse(slot.contains("u1"))
        assertEquals(SecretVault.slot("LUKS:u1"), slot)
        assertNotNull(VaultBlob.decode(blob))
        assertTrue(keys.keys.keys.single().startsWith("usbmem.vault."))
    }

    @Test fun eachVolumeHasItsOwnKeyAndSlot() {
        vault.remember("LUKS:a", pw, "t", "s")
        vault.remember("BitLocker:b", "other".toByteArray(), "t", "s")
        assertEquals(2, store.m.size)
        assertEquals(2, keys.keys.size)
        assertEquals("other", String((vault.recall("BitLocker:b", "t", "s") as RecallResult.Secret).bytes))
        assertArrayEquals(pw, (vault.recall("LUKS:a", "t", "s") as RecallResult.Secret).bytes)
    }

    @Test fun cancelledOrFailedAuthenticationSavesNothing() {
        auth.mode = 1
        assertEquals(RememberResult.Cancelled, vault.remember("LUKS:u1", pw, "t", "s"))
        assertFalse(vault.has("LUKS:u1")); assertTrue(keys.keys.isEmpty())
        auth.mode = 2
        assertTrue(vault.remember("LUKS:u1", pw, "t", "s") is RememberResult.Failed)
        assertFalse(vault.has("LUKS:u1")); assertTrue(keys.keys.isEmpty())
    }

    @Test fun cancelledRecallKeepsTheMemory() {
        vault.remember("LUKS:u1", pw, "t", "s")
        auth.mode = 1
        assertEquals(RecallResult.Cancelled, vault.recall("LUKS:u1", "t", "s"))
        assertTrue(vault.has("LUKS:u1"))   // 取り消しは記憶を壊さない (もう一度認証できる)
        auth.mode = 2
        assertTrue(vault.recall("LUKS:u1", "t", "s") is RecallResult.Failed)
        assertTrue(vault.has("LUKS:u1"))
        auth.mode = 0
        assertTrue(vault.recall("LUKS:u1", "t", "s") is RecallResult.Secret)
    }

    @Test fun invalidatedKeyDiscardsTheMemory() {
        vault.remember("LUKS:u1", pw, "t", "s")
        keys.invalidated = true
        assertEquals(RecallResult.Invalidated, vault.recall("LUKS:u1", "t", "s"))
        assertFalse(vault.has("LUKS:u1")); assertTrue(keys.keys.isEmpty())
        keys.invalidated = false
        assertEquals(RecallResult.NotRemembered, vault.recall("LUKS:u1", "t", "s"))
    }

    @Test fun missingKeyDiscardsTheMemory() {
        vault.remember("LUKS:u1", pw, "t", "s")
        keys.keys.clear()   // 鍵だけ消えた (別の端末への復元・画面ロックの解除)
        assertEquals(RecallResult.Invalidated, vault.recall("LUKS:u1", "t", "s"))
        assertFalse(vault.has("LUKS:u1"))
    }

    @Test fun tamperedOrCorruptBlobIsDiscarded() {
        vault.remember("LUKS:u1", pw, "t", "s")
        val slot = SecretVault.slot("LUKS:u1")
        val b = store.m.getValue(slot)
        b[b.size - 1] = (b[b.size - 1].toInt() xor 1).toByte()
        store.m[slot] = b
        assertEquals(RecallResult.Invalidated, vault.recall("LUKS:u1", "t", "s"))
        assertFalse(vault.has("LUKS:u1"))
        store.m[slot] = byteArrayOf(9, 9, 9)   // 形式が違う
        assertEquals(RecallResult.Invalidated, vault.recall("LUKS:u1", "t", "s"))
        assertFalse(vault.has("LUKS:u1"))
    }

    @Test fun blobMovedToAnotherVolumeDoesNotDecrypt() {
        // 暗号文を別のボリュームの slot に写しても (鍵も写したとして)、AAD (ボリューム ID) が違うので復号できない
        vault.remember("LUKS:a", pw, "t", "s")
        store.m[SecretVault.slot("LUKS:b")] = store.m.getValue(SecretVault.slot("LUKS:a"))
        keys.keys[SecretVault.alias("LUKS:b")] = keys.keys.getValue(SecretVault.alias("LUKS:a"))
        assertEquals(RecallResult.Invalidated, vault.recall("LUKS:b", "t", "s"))
        assertTrue(vault.has("LUKS:a"))   // 元の記憶は無事
    }

    @Test fun forgetRemovesBlobAndKeyAndIsIdempotent() {
        vault.remember("LUKS:u1", pw, "t", "s")
        vault.forget("LUKS:u1")
        assertFalse(vault.has("LUKS:u1")); assertTrue(keys.keys.isEmpty()); assertTrue(store.m.isEmpty())
        vault.forget("LUKS:u1")   // 2 回目も何も起きない
        assertEquals(RecallResult.NotRemembered, vault.recall("LUKS:u1", "t", "s"))
    }

    @Test fun emptyInputsAreRefused() {
        assertTrue(vault.remember("", pw, "t", "s") is RememberResult.Failed)
        assertTrue(vault.remember("LUKS:u1", ByteArray(0), "t", "s") is RememberResult.Failed)
        assertTrue(store.m.isEmpty())
    }

    @Test fun blobCodecRejectsMalformedInput() {
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val ct = ByteArray(20) { it.toByte() }
        val (i2, c2) = VaultBlob.decode(VaultBlob.encode(iv, ct))!!
        assertArrayEquals(iv, i2); assertArrayEquals(ct, c2)
        assertNull(VaultBlob.decode(ByteArray(0)))
        assertNull(VaultBlob.decode(byteArrayOf(2, 12) + iv + ct))                   // 未知のバージョン
        assertNull(VaultBlob.decode(byteArrayOf(1, 4) + iv + ct))                    // IV 長が不正
        assertNull(VaultBlob.decode(byteArrayOf(1, 12) + iv + ByteArray(5)))         // GCM のタグより短い
    }

    @Test fun volumeIdCombinesKindAndId() {
        val base = CryptoInfo(CryptoKind.Luks, "LUKS2", "aes-xts-plain64", true, true, false, true, false, false, false, "")
        assertEquals("", SecretVault.idOf(base))
        assertEquals("Luks:1234", SecretVault.idOf(base.copy(volumeId = "1234")))
        assertTrue(SecretVault.idOf(base.copy(kind = CryptoKind.BitLocker, volumeId = "1234")) != SecretVault.idOf(base.copy(volumeId = "1234")))
    }

    @Test fun infoJsonCarriesVolumeId() {
        val j = """{"kind":2,"format":"LUKS2","cipher":"x","supported":true,"password":true,"volume_id":"abcd-1"}""".toByteArray()
        assertEquals("abcd-1", CryptoInfo.parse(j).volumeId)
        assertEquals("", CryptoInfo.parse("""{"kind":1,"format":"BitLocker"}""".toByteArray()).volumeId)
    }
}
