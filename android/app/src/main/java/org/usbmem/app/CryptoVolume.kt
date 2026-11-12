package org.usbmem.app

import org.json.JSONObject

/**
 * 暗号ボリューム (BitLocker / LUKS1 / LUKS2) を読み取り専用で復号する層への入口 (docs/crypto-volumes.md)。
 *
 * 流れ: 生デバイス/パーティションのハンドル (NativeBridge.nativeOpenFd / nativeOpenUsbMsc / パーティション切り出し) →
 * [CryptoVolume.probe] で暗号化の種類と使える解錠方法を知る → [CryptoVolume.open] で資格情報を渡して復号済みデバイスのハンドルを得る →
 * そのハンドルを既存の NativeBridge.nativeNtfsMountDevice (他の FS も同様) に渡す。
 *
 * 資格情報は **ByteArray** (UTF-8) で扱い、String にしない (String は JVM ヒープに残り消せない)。open は渡された配列を
 * 呼び出し後にゼロで埋める。Android Keystore による資格情報の保存は設計メモのみで未実装 (docs/crypto-volumes.md「Keystore」節)。
 */
object CryptoNative {
    init {
        System.loadLibrary("usbmem_jni")
    }

    @JvmStatic external fun nativeProbe(handle: Long, errorOut: IntArray?): ByteArray?
    @JvmStatic external fun nativeOpen(
        handle: Long, kind: Int, password: ByteArray?, recoveryKey: ByteArray?, keyFile: ByteArray?, flags: Int, infoOut: Array<ByteArray?>?,
    ): Long
}

enum class CryptoKind(val value: Int) { Auto(0), BitLocker(1), Luks(2) }

/** 資格情報なしで分かる情報 (UI の分岐用)。秘密は含まない。 */
data class CryptoInfo(
    val kind: CryptoKind,
    val format: String,
    val cipher: String,
    val supported: Boolean,
    val canPassword: Boolean,
    val canRecoveryKey: Boolean,
    val canKeyFile: Boolean,
    val unlockedWithoutSecret: Boolean,
    val tpmOnly: Boolean,
    val encryptionInProgress: Boolean,
    val note: String,
) {
    companion object {
        fun parse(json: ByteArray): CryptoInfo {
            val o = JSONObject(String(json, Charsets.UTF_8))
            return CryptoInfo(
                kind = when (o.optInt("kind")) { 1 -> CryptoKind.BitLocker; 2 -> CryptoKind.Luks; else -> CryptoKind.Auto },
                format = o.optString("format"), cipher = o.optString("cipher"), supported = o.optBoolean("supported"),
                canPassword = o.optBoolean("password"), canRecoveryKey = o.optBoolean("recovery"), canKeyFile = o.optBoolean("key_file"),
                unlockedWithoutSecret = o.optBoolean("no_secret"), tpmOnly = o.optBoolean("tpm_only"),
                encryptionInProgress = o.optBoolean("in_progress"), note = o.optString("note"),
            )
        }
    }
}

/** 解錠の失敗。code は usbmem::Errc (12 = AuthFailed = 資格情報違い、9 = Unsupported = 非対応の構成、他は壊れ/IO)。 */
class CryptoException(val code: Int, val info: CryptoInfo?, message: String) : Exception(message) {
    val authFailed: Boolean get() = code == AUTH_FAILED
    val unsupported: Boolean get() = code == UNSUPPORTED

    companion object {
        const val UNSUPPORTED = 9
        const val AUTH_FAILED = 12
    }
}

object CryptoVolume {
    /** 暗号ボリュームでなければ null (種類の判定のみ。資格情報は使わない)。 */
    fun probe(deviceHandle: Long): CryptoInfo? {
        val err = IntArray(1)
        val json = CryptoNative.nativeProbe(deviceHandle, err) ?: return if (err[0] == CryptoException.UNSUPPORTED) null
        else throw CryptoException(err[0], null, "probe failed: ${NativeBridge.nativeErrcName(err[0])}")
        return CryptoInfo.parse(json)
    }

    /**
     * 解錠して復号済み (読み取り専用) デバイスのハンドルを返す。ハンドルは NativeBridge.nativeClose で閉じる (元の deviceHandle より先に)。
     * password / recoveryKey / keyFile は呼び出し後にゼロで埋められる (null 可)。失敗は [CryptoException]。
     */
    fun open(
        deviceHandle: Long, kind: CryptoKind = CryptoKind.Auto, password: ByteArray? = null, recoveryKey: ByteArray? = null,
        keyFile: ByteArray? = null, allowPartialEncryption: Boolean = false,
    ): Long {
        val info = arrayOfNulls<ByteArray>(1)
        try {
            val h = CryptoNative.nativeOpen(deviceHandle, kind.value, password, recoveryKey, keyFile, if (allowPartialEncryption) 1 else 0, info)
            if (Handle.valid(h)) return h
            val code = (-h).toInt()
            val parsed = info[0]?.let { runCatching { CryptoInfo.parse(it) }.getOrNull() }
            throw CryptoException(code, parsed, "unlock failed: ${NativeBridge.nativeErrcName(code)}")
        } finally {
            password?.fill(0)
            recoveryKey?.fill(0)
            keyFile?.fill(0)
        }
    }
}
