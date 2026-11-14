package org.usbmem.app

import org.json.JSONObject

/**
 * 暗号ボリューム (BitLocker / LUKS1 / LUKS2) を復号する層への入口 (docs/crypto-volumes.md)。既定は読み取り専用。書き込みは
 * [CryptoVolume.open] の `writable = true` で明示したときだけ (ビルドが USBMEM_NTFS_WRITE / USBMEM_CRYPT_WRITE で、構成が書き込み対応のときに限る。
 * 書き込み可で開けたかは [CryptoVolume.openWithInfo] の `info.writable` で確かめる)。
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
    /** この構成・ビルドなら書き込み可で開ける (資格情報・下位デバイスの書き込み可否は別)。 */
    val writeSupported: Boolean = false,
    /** open の結果: 返したデバイスは書き込み可。probe では常に false。 */
    val writable: Boolean = false,
    /** 書き込み不可の理由 (人に見せてよい。秘密を含まない)。 */
    val writeNote: String = "",
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
                writeSupported = o.optBoolean("write_supported"), writable = o.optBoolean("writable"), writeNote = o.optString("write_note"),
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

    /** [openWithInfo] の結果: 復号済みデバイスのハンドルと、open が返した情報 (writable / writeNote を含む)。 */
    class Opened(val handle: Long, val info: CryptoInfo?)

    /**
     * 解錠して復号済みデバイスのハンドルを返す。ハンドルは NativeBridge.nativeClose で閉じる (元の deviceHandle より先に)。
     * password / recoveryKey / keyFile は呼び出し後にゼロで埋められる (null 可)。失敗は [CryptoException]。
     * [writable] = true は「書き込み可で開く」の明示的な要求 (既定 false = 読み取り専用)。要求しても、ビルド・構成・下位デバイスが
     * 書き込み非対応なら読み取り専用で開く (解錠は成功)。結果は [openWithInfo] の `info.writable` / `info.writeNote` で確かめる。
     * 上に載せる FS を書き込みでマウントするのは、`info.writable == true` のときだけにすること。
     */
    fun open(
        deviceHandle: Long, kind: CryptoKind = CryptoKind.Auto, password: ByteArray? = null, recoveryKey: ByteArray? = null,
        keyFile: ByteArray? = null, allowPartialEncryption: Boolean = false, writable: Boolean = false,
    ): Long = openWithInfo(deviceHandle, kind, password, recoveryKey, keyFile, allowPartialEncryption, writable).handle

    fun openWithInfo(
        deviceHandle: Long, kind: CryptoKind = CryptoKind.Auto, password: ByteArray? = null, recoveryKey: ByteArray? = null,
        keyFile: ByteArray? = null, allowPartialEncryption: Boolean = false, writable: Boolean = false,
    ): Opened {
        val info = arrayOfNulls<ByteArray>(1)
        try {
            val flags = (if (allowPartialEncryption) FLAG_ALLOW_PARTIAL else 0) or (if (writable) FLAG_WRITABLE else 0)
            val h = CryptoNative.nativeOpen(deviceHandle, kind.value, password, recoveryKey, keyFile, flags, info)
            val parsed = info[0]?.let { runCatching { CryptoInfo.parse(it) }.getOrNull() }
            if (Handle.valid(h)) return Opened(h, parsed)
            val code = (-h).toInt()
            throw CryptoException(code, parsed, "unlock failed: ${NativeBridge.nativeErrcName(code)}")
        } finally {
            password?.fill(0)
            recoveryKey?.fill(0)
            keyFile?.fill(0)
        }
    }

    // usbmem_transform_open の flags (c_api_transform.h)
    private const val FLAG_ALLOW_PARTIAL = 1
    private const val FLAG_WRITABLE = 2
}
