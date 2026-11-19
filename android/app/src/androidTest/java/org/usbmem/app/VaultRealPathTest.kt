package org.usbmem.app

import android.app.KeyguardManager
import android.content.pm.PackageManager
import android.hardware.biometrics.BiometricManager
import android.os.Build
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

/**
 * パスワードの記憶の「偽でない」経路 (本物の framework: BiometricManager / KeyguardManager) の試験。docs/crypto-volumes.md「USE_BIOMETRIC」。
 *
 * CryptoUiTest の vault_* は `VaultProvider.override` に偽の vault を差すので、AndroidManifest に USE_BIOMETRIC が無くて
 * `BiometricManager.canAuthenticate` が SecurityException を投げても気づけなかった (SecretVault.supportState() は例外を Unavailable に丸める)。
 * ここでは override を使わず、アプリ (計装の対象アプリのプロセス) の権限で本物を呼び、例外が出ないこと・端末の状態と判定が合うことを確かめる。
 * 画面ロックを設定していない CI のエミュレータでは NoCredential、AVD で PIN を設定してあれば Supported (どちらでも通る)。
 */
class VaultRealPathTest {
    private val inst = InstrumentationRegistry.getInstrumentation()
    private val ctx = inst.targetContext

    @Before fun setUp() { VaultProvider.override = null }
    @After fun tearDown() { VaultProvider.override = null }

    @Test
    fun manifestDeclaresUseBiometric() {
        assertEquals("USE_BIOMETRIC がマニフェストに無い (パスワードの記憶が一切出なくなる)",
            PackageManager.PERMISSION_GRANTED, ctx.packageManager.checkPermission("android.permission.USE_BIOMETRIC", ctx.packageName))
    }

    @Test
    fun canAuthenticateReturnsWithoutSecurityException() {
        assumeTrue("API 30 未満では BiometricManager.Authenticators の組み合わせを使わない", Build.VERSION.SDK_INT >= 30)
        val bm = ctx.getSystemService(BiometricManager::class.java)
        assertNotNull(bm)
        val rc = bm.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL)   // 権限が無いと SecurityException
        assertTrue("unexpected canAuthenticate result $rc", rc in listOf(
            BiometricManager.BIOMETRIC_SUCCESS, BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED, BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE,
            BiometricManager.BIOMETRIC_ERROR_HW_UNAVAILABLE, BiometricManager.BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED))
    }

    @Test
    fun supportStateMatchesDeviceLockState() {
        assumeTrue(Build.VERSION.SDK_INT >= 30)
        // SecretVault を通さずに呼ぶ (通すと例外が Unavailable に丸められて見えなくなる)
        val direct = VaultProvider.supportState(ctx)
        // 本番の経路 (VaultProvider.get = 本物の AndroidKeystoreKeys + BiometricUserAuth + PrefsVaultStore) でも同じ
        val viaVault = VaultProvider.get(ctx).supportState()
        assertEquals(direct, viaVault)
        val secure = ctx.getSystemService(KeyguardManager::class.java).isDeviceSecure
        if (!secure) assertEquals("画面ロック未設定のエミュレータでは NoCredential (例外・Unavailable ではない)", VaultSupport.NoCredential, direct)
        else assertEquals(VaultSupport.Supported, direct)
    }
}
