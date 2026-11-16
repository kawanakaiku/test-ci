package org.usbmem.app

import java.security.MessageDigest
import javax.crypto.Cipher

/**
 * 暗号ボリュームのパスワードの記憶 (オプトイン)。docs/crypto-volumes.md「Android Keystore」節。
 *
 * このファイルは Android の API に依存しない (純 JVM でテストできる)。鍵の発行 ([VaultKeys])・ユーザー認証 ([UserAuth])・暗号文の置き場所 ([VaultStore]) を
 * 差し込む形にして、本番は AndroidKeyStore + BiometricPrompt + SharedPreferences (KeystoreVault.kt)、試験は認証なしの Keystore か純 JVM の鍵を使う。
 *
 * 保存するのは**パスワード (パスフレーズ) だけ**。回復キー・BEK ファイル・復号後のキー (VMK / FVEK / マスターキー) は対象外。
 * 平文を永続化しない: 保存するのは [VaultBlob] (IV + AES-256-GCM の暗号文) だけで、鍵は端末の外に出ない。ボリュームごとに別の鍵 (別名) を使い、
 * 暗号文にはボリュームの識別子を AAD として結びつける (別のボリュームの暗号文に差し替えても復号できない)。
 *
 * 呼び出しは**ブロッキング** (認証の画面が閉じるまで待つ) なので、メインスレッドから呼ばない。返ったバイト列 ([RecallResult.Secret]) は呼び出し側がゼロ埋めする。
 */

/** 鍵が使えなくなっている (生体登録の変更・画面ロックの解除・別の端末への復元などで消えた / 無効化された) ことを表す。保存してあった記憶は捨てる。 */
class KeyInvalidatedException(message: String? = null) : Exception(message)

/** ボリュームごとの AES-256-GCM 鍵の発行と、それに結びついた [Cipher] の用意。 */
interface VaultKeys {
    /** [alias] の鍵を (作り直して) 暗号化用 [Cipher] を返す。IV は Cipher が決める (使い回さない)。認証が要る鍵なら、まだ認証されていない Cipher を返す。 */
    fun encryptCipher(alias: String): Cipher

    /** [alias] の鍵で、[iv] を使う復号用 [Cipher] を返す。鍵が無い・無効化されているときは [KeyInvalidatedException]。 */
    fun decryptCipher(alias: String, iv: ByteArray): Cipher

    /** [alias] の鍵を消す (無ければ何もしない)。 */
    fun delete(alias: String)

    /** [e] が「鍵の無効化」を表す例外か (プラットフォーム固有の例外を [KeyInvalidatedException] と同じに扱う)。 */
    fun isInvalidation(e: Throwable): Boolean = e is KeyInvalidatedException
}

/** 暗号文 ([VaultBlob]) の置き場所 (アプリ専用領域)。 */
interface VaultStore {
    fun get(slot: String): ByteArray?
    fun put(slot: String, blob: ByteArray)
    fun remove(slot: String)
    /** 保存されている slot の名前 (試験・診断用。中身は出さない)。 */
    fun slots(): Set<String>
}

sealed class AuthResult {
    /** 認証できた。[cipher] は認証済み (これで doFinal できる)。 */
    class Ok(val cipher: Cipher) : AuthResult()
    /** 利用者が取り消した。 */
    object Cancelled : AuthResult()
    /** 認証できなかった (ロックアウト・ハードウェアの不具合など)。[message] は人に見せてよい。 */
    class Failed(val message: String) : AuthResult()
}

/** 生体認証 / 画面ロックで [cipher] を認証する。画面が閉じるまで戻らない (メインスレッド以外で呼ぶ)。 */
interface UserAuth {
    fun authenticate(cipher: Cipher, title: String, subtitle: String): AuthResult
}

sealed class RememberResult {
    object Saved : RememberResult()
    object Cancelled : RememberResult()
    class Failed(val message: String) : RememberResult()
}

sealed class RecallResult {
    /** 取り出せたパスワード (UTF-8)。呼び出し側がゼロ埋めする。 */
    class Secret(val bytes: ByteArray) : RecallResult()
    object Cancelled : RecallResult()
    /** 記憶が使えなかった (鍵の無効化・暗号文の破損・差し替え)。**記憶は破棄済み**。通常のパスワード入力に戻る。 */
    object Invalidated : RecallResult()
    object NotRemembered : RecallResult()
    class Failed(val message: String) : RecallResult()
}

/** 暗号文の保存形式: `[version=1][ivLen][iv][ciphertext+tag]`。 */
object VaultBlob {
    const val VERSION: Byte = 1
    private const val TAG = 16

    fun encode(iv: ByteArray, ct: ByteArray): ByteArray {
        require(iv.size in 12..32)
        val out = ByteArray(2 + iv.size + ct.size)
        out[0] = VERSION; out[1] = iv.size.toByte()
        System.arraycopy(iv, 0, out, 2, iv.size)
        System.arraycopy(ct, 0, out, 2 + iv.size, ct.size)
        return out
    }

    /** (iv, 暗号文) に分ける。形式が合わなければ null。 */
    fun decode(b: ByteArray): Pair<ByteArray, ByteArray>? {
        if (b.size < 2 || b[0] != VERSION) return null
        val n = b[1].toInt() and 0xff
        if (n < 12 || n > 32 || b.size < 2 + n + TAG + 1) return null
        return b.copyOfRange(2, 2 + n) to b.copyOfRange(2 + n, b.size)
    }
}

class SecretVault(
    private val keys: VaultKeys,
    private val auth: UserAuth,
    private val store: VaultStore,
    /** この端末・この時点で記憶を使えるか (OS のバージョン・画面ロック / 生体の登録など) */
    private val supported: () -> Boolean = { true },
) {
    fun isSupported(): Boolean = try { supported() } catch (_: Exception) { false }

    fun has(id: String): Boolean = store.get(slot(id)) != null

    /**
     * [secret] (パスワード) を、[id] のボリューム用の鍵で暗号化して保存する。認証が要る (鍵を使う操作は認証済みの Cipher でしかできない)。
     * 取り消し・失敗したときは何も残さない (前の記憶も、鍵を作り直したので使えないため消す)。[secret] は消さない (呼び出し側が消す)。
     */
    fun remember(id: String, secret: ByteArray, title: String, subtitle: String): RememberResult {
        if (id.isEmpty() || secret.isEmpty()) return RememberResult.Failed("empty")
        return try {
            val c = keys.encryptCipher(alias(id))
            when (val a = auth.authenticate(c, title, subtitle)) {
                is AuthResult.Cancelled -> { forget(id); RememberResult.Cancelled }
                is AuthResult.Failed -> { forget(id); RememberResult.Failed(a.message) }
                is AuthResult.Ok -> {
                    a.cipher.updateAAD(aad(id))
                    val ct = a.cipher.doFinal(secret)
                    store.put(slot(id), VaultBlob.encode(a.cipher.iv, ct))
                    RememberResult.Saved
                }
            }
        } catch (e: Exception) {
            forget(id)
            RememberResult.Failed(e.javaClass.simpleName)
        }
    }

    /** [id] の記憶を認証つきで取り出す。鍵が無効・暗号文が壊れているときは記憶を破棄して [RecallResult.Invalidated]。 */
    fun recall(id: String, title: String, subtitle: String): RecallResult {
        val blob = store.get(slot(id)) ?: return RecallResult.NotRemembered
        val (iv, ct) = VaultBlob.decode(blob) ?: run { forget(id); return RecallResult.Invalidated }
        try {
            val c = try { keys.decryptCipher(alias(id), iv) } catch (e: Exception) {
                if (keys.isInvalidation(e)) { forget(id); return RecallResult.Invalidated }
                return RecallResult.Failed(e.javaClass.simpleName)
            }
            return when (val a = auth.authenticate(c, title, subtitle)) {
                is AuthResult.Cancelled -> RecallResult.Cancelled
                is AuthResult.Failed -> RecallResult.Failed(a.message)
                is AuthResult.Ok -> {
                    try {
                        a.cipher.updateAAD(aad(id))
                        RecallResult.Secret(a.cipher.doFinal(ct))
                    } catch (e: Exception) {
                        // 鍵の無効化・暗号文の改ざん / 破損 (GCM のタグ不一致): 使えない記憶は捨てる
                        if (keys.isInvalidation(e) || e is javax.crypto.AEADBadTagException || e is javax.crypto.BadPaddingException) { forget(id); RecallResult.Invalidated }
                        else RecallResult.Failed(e.javaClass.simpleName)
                    }
                }
            }
        } catch (e: Exception) {
            return RecallResult.Failed(e.javaClass.simpleName)
        }
    }

    /** 記憶を消す (暗号文と鍵)。無くても何もしない。 */
    fun forget(id: String) {
        try { store.remove(slot(id)) } catch (_: Exception) { }
        try { keys.delete(alias(id)) } catch (_: Exception) { }
    }

    companion object {
        private fun digest(id: String): String {
            val h = MessageDigest.getInstance("SHA-256").digest(("usbmem-vault-v1:$id").toByteArray(Charsets.UTF_8))
            val sb = StringBuilder()
            for (i in 0 until 16) sb.append("%02x".format(h[i].toInt() and 0xff))
            return sb.toString()
        }
        /** 暗号文の置き場所の名前 (ボリューム ID のハッシュ。ID そのものは載せない)。 */
        fun slot(id: String): String = "v1_" + digest(id)
        /** Keystore の鍵の別名。 */
        fun alias(id: String): String = "usbmem.vault." + digest(id)
        private fun aad(id: String): ByteArray = ("usbmem-vault-v1:$id").toByteArray(Charsets.UTF_8)

        /** 記憶の鍵 (ボリューム ID)。識別子が取れないボリュームは空 (記憶の対象外)。 */
        fun idOf(info: CryptoInfo): String = if (info.volumeId.isBlank()) "" else "${info.kind.name}:${info.volumeId}"
    }
}
