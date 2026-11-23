// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.smb

import org.json.JSONArray
import org.json.JSONObject
import java.security.SecureRandom

/**
 * SMB の最小バージョン (許容範囲の下限。上限は常に SMB 3.1.1)。**SMB1 は選べない** (SMBJ が SMB1 を実装していないので、そもそも話せない)。docs/smb.md
 */
enum class SmbMinVersion(val key: String) {
    V202("2.0.2"), V21("2.1"), V30("3.0"), V311("3.1.1");

    companion object {
        fun of(key: String?): SmbMinVersion = entries.firstOrNull { it.key == key } ?: V21
    }
}

/**
 * 保存する SMB 接続の設定。**パスワードは持たない** (保存するときは Android Keystore で暗号化して別に置く: [SmbPasswords])。
 * [id] = 端末内の識別子 (ランダム。ホスト名や共有名を含まない)。SAF のルート・ドキュメント ID・Keystore の別名のもとになる。
 */
data class SmbConnection(
    val id: String,
    val name: String,
    val host: String,
    val port: Int = DEFAULT_PORT,
    val share: String,
    val domain: String = "",
    val user: String = "",
    val minVersion: SmbMinVersion = SmbMinVersion.V21,
    /** 暗号化 (SMB 3 の AES) を必須にする。サーバーが対応しなければ接続できない */
    val requireEncryption: Boolean = false,
    /** 署名を必須にする (サーバーが求めるときは、この指定が無くても署名する) */
    val requireSigning: Boolean = false,
) {
    /** 空のユーザー名 = ゲスト (匿名) で接続する */
    val isGuest: Boolean get() = user.isEmpty()

    /** パスワードを含まない人向けの表示 ("host:port/share") */
    fun target(): String = (if (':' in host) "[$host]" else host) + (if (port != DEFAULT_PORT) ":$port" else "") + "/" + share

    fun toJson(): JSONObject = JSONObject()
        .put("id", id).put("name", name).put("host", host).put("port", port).put("share", share)
        .put("domain", domain).put("user", user).put("minVersion", minVersion.key)
        .put("requireEncryption", requireEncryption).put("requireSigning", requireSigning)

    /** 入力の誤り。null = 問題なし */
    fun validate(): SmbInputError? = validateFields(name, host, port, share, domain, user)

    companion object {
        const val DEFAULT_PORT = 445
        const val MAX_NAME = 64

        fun newId(): String {
            val b = ByteArray(6); SecureRandom().nextBytes(b)
            return b.joinToString("") { "%02x".format(it) }
        }

        fun fromJson(o: JSONObject): SmbConnection? = try {
            SmbConnection(
                id = o.getString("id"), name = o.getString("name"), host = o.getString("host"), port = o.optInt("port", DEFAULT_PORT),
                share = o.getString("share"), domain = o.optString("domain", ""), user = o.optString("user", ""),
                minVersion = SmbMinVersion.of(o.optString("minVersion", "")),
                requireEncryption = o.optBoolean("requireEncryption", false), requireSigning = o.optBoolean("requireSigning", false),
            ).takeIf { it.id.isNotEmpty() && it.validate() == null }
        } catch (_: Exception) { null }

        fun listToJson(l: List<SmbConnection>): String = JSONArray().also { a -> l.forEach { a.put(it.toJson()) } }.toString()

        /** 壊れた要素・検証に通らない要素は黙って捨てる (保存データの手書き編集・将来の形式変更で、画面が開けなくならないように) */
        fun listFromJson(s: String?): List<SmbConnection> {
            if (s.isNullOrEmpty()) return emptyList()
            return try {
                val a = JSONArray(s)
                (0 until a.length()).mapNotNull { i -> a.optJSONObject(i)?.let { fromJson(it) } }
            } catch (_: Exception) { emptyList() }
        }

        /** 入力の検査。ホスト名・共有名は区切り文字・制御文字・空白を許さない (パスの混入・表示の崩れを防ぐ) */
        fun validateFields(name: String, host: String, port: Int, share: String, domain: String, user: String): SmbInputError? {
            if (name.isBlank() || name.length > MAX_NAME || name.any { it.code < 0x20 }) return SmbInputError.Name
            if (host.isEmpty() || host.length > 253 || host.any { it.isWhitespace() || it.code < 0x20 || it == '/' || it == '\\' || it == '@' }) return SmbInputError.Host
            if (port !in 1..65535) return SmbInputError.Port
            if (share.isEmpty() || share.length > 80 || share.any { it.code < 0x20 || it in "/\\:*?\"<>|" }) return SmbInputError.Share
            if (domain.length > 255 || domain.any { it.code < 0x20 || it == '\\' || it == '/' }) return SmbInputError.Domain
            if (user.length > 255 || user.any { it.code < 0x20 || it == '\\' || it == '/' }) return SmbInputError.User
            return null
        }
    }
}

/** 入力の誤りの種類 (画面が文言に直す) */
enum class SmbInputError { Name, Host, Port, Share, Domain, User }
