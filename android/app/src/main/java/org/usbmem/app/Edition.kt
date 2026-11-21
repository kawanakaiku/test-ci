package org.usbmem.app

import org.json.JSONObject

/**
 * エディション (docs/editions.md)。個人版 (personal) はすべてのファイルシステムを含む自分用のビルドで、配布しない。
 * 公開版 (public) は GPL 系のコード (Linux カーネル由来のドライバなど) を一切含まないビルドで、扱えるファイルシステムがネイティブ層に入っているものだけになる。
 *
 * 入っているファイルシステムの一覧は、実行時にネイティブ層へ問い合わせる ([NativeBridge.nativeEditionInfo]。パーティション一覧の fs 文字列と同じ名前)。
 * 個人版では常に「全部対応」を返すので、個人版の挙動はこのクラスの有無で変わらない。
 */
object Edition {
    val isPublic: Boolean = BuildConfig.EDITION == "public"

    /** ネイティブ層 (jni_ntfs.cpp) が「判別できたが、この版に入っていないファイルシステム」で返す errno (EPROTONOSUPPORT = 93)。[NativeException.code] に入る。 */
    const val UNSUPPORTED_CODE = 93

    // 公開版だけ、ネイティブ層に問い合わせて持つ。取れなければ空 (= どのファイルシステムもマウントしない。安全側)。
    private val publicFs: Set<String> by lazy {
        runCatching { parseFs(String(NativeBridge.nativeEditionInfo(), Charsets.UTF_8)) }.getOrDefault(emptySet())
    }

    /** この版のネイティブ層が [fs] ("ntfs" / "f2fs" ... パーティション一覧の fs 文字列) をマウントできるか。個人版は常に true。 */
    fun supports(fs: String): Boolean = !isPublic || fs in publicFs

    /** この版が扱えるファイルシステムの表示用の文字列 ("F2FS" など)。個人版は空。 */
    fun supportedFsLabel(): String = if (!isPublic) "" else publicFs.map { it.uppercase() }.sorted().joinToString(" / ").ifEmpty { "-" }

    /** ネイティブの `{"edition":"public","fs":["f2fs"]}` から fs の集合を取り出す。 */
    internal fun parseFs(json: String): Set<String> {
        val a = JSONObject(json).getJSONArray("fs")
        return List(a.length()) { a.getString(it) }.toSet()
    }
}
