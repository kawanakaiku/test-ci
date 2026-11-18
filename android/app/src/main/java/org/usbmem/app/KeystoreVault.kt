package org.usbmem.app

import android.app.Activity
import android.content.Context
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.CancellationSignal
import android.os.Looper
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.Base64
import java.lang.ref.WeakReference
import java.security.KeyStore
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * [SecretVault] の Android 実装 (AndroidKeyStore の AES-256-GCM 鍵 + framework の BiometricPrompt + SharedPreferences)。docs/crypto-volumes.md「Android Keystore」節。
 *
 * - 鍵: ボリュームごとに別名で発行。[requireAuth] = true (本番) は `setUserAuthenticationRequired(true)` + 使うたびに認証 (生体 BIOMETRIC_STRONG または画面ロック。API 30+)。
 *   StrongBox があれば使い (`setIsStrongBoxBacked`)、使えなければ通常の TEE / ソフトの Keystore に落とす。生体の登録が変わると鍵は無効化される ([KeyPermanentlyInvalidatedException])。
 *   [requireAuth] = false は計装テスト専用 (認証なしの同じ Keystore 経路で、鍵の発行・暗号化・復号・削除を試す)。
 * - 暗号文: SharedPreferences ([PrefsVaultStore])。平文は書かない。
 */
class AndroidKeystoreKeys(private val requireAuth: Boolean = true) : VaultKeys {
    private fun ks(): KeyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }

    private fun generate(alias: String, strongBox: Boolean) {
        val b = KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
        if (requireAuth) {
            if (Build.VERSION.SDK_INT < 30) throw UnsupportedOperationException("auth-bound key needs API 30")
            b.setUserAuthenticationRequired(true)
            b.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL)   // 0 = 操作ごとに認証 (CryptoObject に結びつく)
            b.setInvalidatedByBiometricEnrollment(true)
        }
        if (strongBox && Build.VERSION.SDK_INT >= 28) b.setIsStrongBoxBacked(true)
        val g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        g.init(b.build())
        g.generateKey()
    }

    override fun encryptCipher(alias: String): Cipher {
        val k = ks()
        if (k.containsAlias(alias)) k.deleteEntry(alias)
        fun make(strongBox: Boolean): Cipher {
            generate(alias, strongBox)
            val key = ks().getKey(alias, null) as SecretKey
            return Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key) }
        }
        if (Build.VERSION.SDK_INT < 28) return make(false)
        return try { make(true) } catch (e: Exception) {
            // StrongBox が無い / 使えない端末 (StrongBoxUnavailableException、または StrongBox の初期化失敗) は、通常の Keystore で作り直す
            runCatching { ks().deleteEntry(alias) }
            make(false)
        }
    }

    override fun decryptCipher(alias: String, iv: ByteArray): Cipher {
        val key = try { ks().getKey(alias, null) as? SecretKey } catch (e: java.security.UnrecoverableKeyException) { null }
            ?: throw KeyInvalidatedException("key missing")
        return try {
            Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, iv)) }
        } catch (e: KeyPermanentlyInvalidatedException) {
            throw KeyInvalidatedException("key invalidated")
        }
    }

    override fun delete(alias: String) {
        val k = ks()
        if (k.containsAlias(alias)) k.deleteEntry(alias)
    }

    override fun isInvalidation(e: Throwable): Boolean = e is KeyInvalidatedException || e is KeyPermanentlyInvalidatedException
}

/** SharedPreferences に Base64 で置く (アプリ専用領域。allowBackup=false なのでバックアップにも出ない)。 */
class PrefsVaultStore(ctx: Context, name: String = "usbmem_vault") : VaultStore {
    private val prefs = ctx.applicationContext.getSharedPreferences(name, Context.MODE_PRIVATE)
    override fun get(slot: String): ByteArray? = prefs.getString(slot, null)?.let { runCatching { Base64.decode(it, Base64.NO_WRAP) }.getOrNull() }
    override fun put(slot: String, blob: ByteArray) { prefs.edit().putString(slot, Base64.encodeToString(blob, Base64.NO_WRAP)).commit() }
    override fun remove(slot: String) { prefs.edit().remove(slot).commit() }
    override fun slots(): Set<String> = prefs.all.keys.toSet()
}

/**
 * framework の BiometricPrompt (API 28+ にあるが、CryptoObject + 画面ロックの併用は API 30+) で、生体 (BIOMETRIC_STRONG) または画面ロックを求める。
 * androidx.biometric を足さないための選択 (docs/crypto-volumes.md)。画面 (Activity) は [activity] で取る。メインスレッド以外から呼ぶ (結果が来るまで待つ)。
 */
class BiometricUserAuth(private val activity: () -> Activity?) : UserAuth {
    override fun authenticate(cipher: Cipher, title: String, subtitle: String): AuthResult {
        if (Build.VERSION.SDK_INT < 30) return AuthResult.Failed("API 30 以上が必要です")
        if (Looper.myLooper() == Looper.getMainLooper()) return AuthResult.Failed("main thread")
        val act = activity() ?: return AuthResult.Failed("画面がありません")
        val latch = CountDownLatch(1)
        val out = AtomicReference<AuthResult>(AuthResult.Failed("timeout"))
        val signal = CancellationSignal()
        act.runOnUiThread {
            try {
                val prompt = BiometricPrompt.Builder(act)
                    .setTitle(title).setSubtitle(subtitle)
                    .setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL)
                    .build()
                prompt.authenticate(BiometricPrompt.CryptoObject(cipher), signal, act.mainExecutor, object : BiometricPrompt.AuthenticationCallback() {
                    override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                        val c = result.cryptoObject?.cipher
                        out.set(if (c != null) AuthResult.Ok(c) else AuthResult.Failed("no crypto object"))
                        latch.countDown()
                    }
                    override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
                        out.set(if (errorCode == ERR_CANCELED || errorCode == ERR_USER_CANCELED || errorCode == ERR_NEGATIVE_BUTTON) AuthResult.Cancelled
                                else AuthResult.Failed(errString?.toString() ?: "error $errorCode"))
                        latch.countDown()
                    }
                })
            } catch (e: Exception) {
                out.set(AuthResult.Failed(e.javaClass.simpleName))
                latch.countDown()
            }
        }
        if (!latch.await(5, TimeUnit.MINUTES)) { act.runOnUiThread { signal.cancel() }; return AuthResult.Failed("timeout") }
        return out.get()
    }

    private companion object {
        // BiometricConstants (BiometricPrompt の onAuthenticationError の errorCode)
        const val ERR_CANCELED = 5
        const val ERR_USER_CANCELED = 10
        const val ERR_NEGATIVE_BUTTON = 13
    }
}

/**
 * アプリで使う [SecretVault] の入口。本番は [AndroidKeystoreKeys] + [BiometricUserAuth] + [PrefsVaultStore]。計装テストは [override] に差し替える
 * (実際の生体認証は emulator の CI では使えないため)。
 */
object VaultProvider {
    @Volatile var override: SecretVault? = null
    @Volatile private var prod: SecretVault? = null
    @Volatile private var activityRef: WeakReference<Activity>? = null

    /** BiometricPrompt を出す画面。MainActivity が onCreate で登録し、onDestroy で外す。 */
    fun attach(a: Activity) { activityRef = WeakReference(a) }
    fun detach(a: Activity) { if (activityRef?.get() === a) activityRef = null }

    fun get(ctx: Context): SecretVault = override ?: prod ?: synchronized(this) { prod ?: create(ctx.applicationContext).also { prod = it } }

    private fun create(app: Context): SecretVault = SecretVault(
        AndroidKeystoreKeys(requireAuth = true), BiometricUserAuth { activityRef?.get() }, PrefsVaultStore(app),
        support = { supportState(app) },
    )

    /** この端末で、生体 (BIOMETRIC_STRONG) または画面ロックが使える (API 30+ で、登録・設定済み)。 */
    fun canAuthenticate(ctx: Context): Boolean = supportState(ctx) == VaultSupport.Supported

    /** [canAuthenticate] の理由つき版 (使えないとき、解錠ダイアログに理由を一言出す)。 */
    fun supportState(ctx: Context): VaultSupport {
        if (Build.VERSION.SDK_INT < 30) return VaultSupport.OsTooOld
        val bm = ctx.getSystemService(BiometricManager::class.java) ?: return VaultSupport.Unavailable
        return when (bm.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL)) {
            BiometricManager.BIOMETRIC_SUCCESS -> VaultSupport.Supported
            BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED -> VaultSupport.NoCredential   // 画面ロックも指紋も未設定
            else -> VaultSupport.Unavailable
        }
    }
}
