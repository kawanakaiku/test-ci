// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import com.hierynomus.security.AEADBlockCipher
import com.hierynomus.security.Cipher
import com.hierynomus.security.DerivationFunction
import com.hierynomus.security.Mac
import com.hierynomus.security.MessageDigest
import com.hierynomus.security.SecurityProvider
import com.hierynomus.security.bc.BCSecurityProvider
import com.hierynomus.security.jce.JceSecurityProvider
import java.util.Random
import javax.crypto.spec.GCMParameterSpec

/**
 * SMBJ の暗号の部品 (署名・暗号化・ハッシュ) を、**端末の JCE (Conscrypt など。ネイティブ・AES 命令)** で行う [SecurityProvider]。
 * 既定の SMBJ は BouncyCastle の軽量 API (純 Java) だけを使うので、署名 (AES-CMAC) や暗号化 (AES-GCM) の CPU 時間が転送の速さを決めうる (docs/smb.md 13 章)。
 *
 * **JCE の結果が BouncyCastle と完全に一致する部品だけを使う** (起動後の最初の利用で、固定のデータで両方を動かして比べる。[probe])。
 * 一致しない・例外を投げる・端末に無い部品 (AES-CMAC は端末による。MD4 / DES / RC4 / AES-CCM は JCE に無い) は、BouncyCastle のまま。
 * なので、この差し替えで通信の内容 (署名値・暗号文) は変わらず、サーバーとの互換も変わらない。
 */
internal class HybridSecurityProvider private constructor(private val ok: Set<String>) : SecurityProvider {
    private val bc: SecurityProvider = BCSecurityProvider()
    private val jce: SecurityProvider = JceSecurityProvider()

    /** JCE で動かす部品の名前 (種類:正規化した名前)。試験・記録用 */
    val jceParts: Set<String> get() = ok

    private fun use(kind: String, name: String) = "$kind:${name.uppercase()}" in ok

    override fun getDigest(name: String): MessageDigest = if (use("digest", name)) jce.getDigest(name) else bc.getDigest(name)
    override fun getMac(name: String): Mac = if (use("mac", name)) jce.getMac(name) else bc.getMac(name)
    override fun getCipher(name: String): Cipher = bc.getCipher(name)
    override fun getAEADBlockCipher(name: String): AEADBlockCipher = if (use("aead", name)) jce.getAEADBlockCipher(name) else bc.getAEADBlockCipher(name)
    override fun getDerivationFunction(name: String): DerivationFunction = bc.getDerivationFunction(name)

    companion object {
        /** 比べる対象 (SMBJ が要求する名前)。AES-CMAC は SMBJ が "AesCmac"、HMAC-SHA256 は "HmacSHA256" で問い合わせる */
        private val DIGESTS = listOf("MD5", "SHA-512")
        private val MACS = listOf("HmacSHA256", "AesCmac")
        private val AEADS = listOf("AES/GCM/NoPadding")

        @Volatile private var cached: HybridSecurityProvider? = null

        /** 一度だけ比べて、結果を覚える */
        fun get(): HybridSecurityProvider = cached ?: synchronized(this) { cached ?: HybridSecurityProvider(probe()).also { cached = it } }

        /** BouncyCastle だけ (差し替えない) */
        fun bcOnly(): SecurityProvider = BCSecurityProvider()

        internal fun probe(): Set<String> {
            val bc: SecurityProvider = BCSecurityProvider()
            val jce: SecurityProvider = JceSecurityProvider()
            val out = HashSet<String>()
            val rnd = Random(0x5eed)
            val data = ByteArray(5000).also { rnd.nextBytes(it) }
            for (n in DIGESTS) if (digestSame(bc, jce, n, data)) out += "digest:${n.uppercase()}"
            for (n in MACS) if (macSame(bc, jce, n, data, rnd)) out += "mac:${n.uppercase()}"
            for (n in AEADS) if (aeadSame(bc, jce, n, data, rnd)) out += "aead:${n.uppercase()}"
            return out
        }

        internal fun digestSame(bc: SecurityProvider, jce: SecurityProvider, name: String, data: ByteArray): Boolean = try {
            val a = jce.getDigest(name); a.update(data, 0, 1); a.update(data[1]); a.update(data, 2, data.size - 2)
            val b = bc.getDigest(name); b.update(data)
            val ra = a.digest(); val rb = b.digest()
            ra.isNotEmpty() && ra.contentEquals(rb) && a.digestLength == b.digestLength
        } catch (_: Throwable) { false }

        internal fun macSame(bc: SecurityProvider, jce: SecurityProvider, name: String, data: ByteArray, rnd: Random): Boolean = try {
            val key = ByteArray(16).also { rnd.nextBytes(it) }
            var same = true
            for (len in intArrayOf(0, 1, 15, 16, 17, 64, 1000, 5000)) {   // ブロック境界・空・大きなデータ
                val a = jce.getMac(name); a.init(key); a.update(data, 0, len / 2); a.update(data, len / 2, len - len / 2)
                val b = bc.getMac(name); b.init(key); b.update(data, 0, len)
                val ra = a.doFinal(); val rb = b.doFinal()
                if (ra.isEmpty() || !ra.contentEquals(rb)) { same = false; break }
                // 同じ Mac をもう一度使う (reset) 場合も一致する
                a.reset(); b.reset(); a.update(data, 0, 33); b.update(data, 0, 33)
                if (!a.doFinal().contentEquals(b.doFinal())) { same = false; break }
            }
            same
        } catch (_: Throwable) { false }

        /** AES-GCM: 暗号化の結果 (暗号文 + タグ) が一致し、復号 (SMBJ の使い方: update の後に doFinal) で元に戻り、改ざんを拒否すること */
        internal fun aeadSame(bc: SecurityProvider, jce: SecurityProvider, name: String, data: ByteArray, rnd: Random): Boolean = try {
            val key = ByteArray(16).also { rnd.nextBytes(it) }
            val iv = ByteArray(12).also { rnd.nextBytes(it) }
            val aad = ByteArray(32).also { rnd.nextBytes(it) }
            var same = true
            for (len in intArrayOf(0, 1, 16, 100, 4096)) {
                val plain = data.copyOf(len)
                fun enc(p: SecurityProvider): ByteArray {
                    val c = p.getAEADBlockCipher(name); c.init(Cipher.CryptMode.ENCRYPT, key, GCMParameterSpec(128, iv)); c.updateAAD(aad, 0, aad.size)
                    return c.doFinal(plain, 0, plain.size)
                }
                val ea = enc(jce); val eb = enc(bc)
                if (ea.size != len + 16 || !ea.contentEquals(eb)) { same = false; break }
                // SMBJ の復号: 暗号文を update に、タグを doFinal に渡す (update が空を返す実装でも、doFinal が全部返す)
                fun dec(p: SecurityProvider, ct: ByteArray): ByteArray? = try {
                    val c = p.getAEADBlockCipher(name); c.init(Cipher.CryptMode.DECRYPT, key, GCMParameterSpec(128, iv)); c.updateAAD(aad, 0, aad.size)
                    val a = c.update(ct, 0, len); val b = c.doFinal(ct, len, 16)
                    (a ?: ByteArray(0)) + b
                } catch (_: Throwable) { null }
                val da = dec(jce, ea)
                if (da == null || !da.contentEquals(plain)) { same = false; break }
                val bad = ea.copyOf(); bad[bad.size - 1] = (bad[bad.size - 1].toInt() xor 1).toByte()
                if (dec(jce, bad) != null) { same = false; break }   // タグの改ざんを受け入れない
            }
            same
        } catch (_: Throwable) { false }
    }
}
