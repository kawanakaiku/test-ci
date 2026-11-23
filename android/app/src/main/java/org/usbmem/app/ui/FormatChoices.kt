// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

/**
 * フォーマット画面の選択肢まわりの純粋な関数 (Android に依存しない。JVM の単体テストで確かめる)。docs/disk-ui.md
 * 作れる形式・容量・クラスタサイズ・ラベルの上限は、ネイティブの問い合わせ ([FormatCap]) の結果をそのまま使う。
 */
object FormatChoices {
    /** 画面の「FAT (自動)」の選択肢の名前 (FAT12 / 16 / 32 のどれかを、容量から選ぶ)。ネイティブには渡さない。 */
    const val FAT_AUTO = "fat-auto"

    /**
     * 容量から FAT の種別を選ぶ。小さい (8 MiB 未満) は FAT12、512 MiB 未満は FAT16、それ以上は FAT32 を第一候補にし、
     * [canMake] (その種別がこの容量で作れるか。ネイティブの問い合わせ) が false なら次の候補へ。どれも作れなければ null。
     */
    fun autoFat(sizeBytes: Long, canMake: (String) -> Boolean): String? {
        val mib = 1L shl 20
        val order = when {
            sizeBytes < 8 * mib -> listOf("fat12", "fat16", "fat32")
            sizeBytes < 512 * mib -> listOf("fat16", "fat32", "fat12")
            else -> listOf("fat32", "fat16", "fat12")
        }
        return order.firstOrNull(canMake)
    }

    /** UTF-8 にしたときのバイト数が [maxBytes] に収まる先頭部分 (文字の途中で切らない)。 */
    fun takeUtf8Bytes(s: String, maxBytes: Int): String {
        var bytes = 0
        var i = 0
        while (i < s.length) {
            val cp = s.codePointAt(i)
            val n = Character.charCount(cp)
            val b = if (cp < 0x80) 1 else if (cp < 0x800) 2 else if (cp < 0x10000) 3 else 4
            if (bytes + b > maxBytes) break
            bytes += b
            i += n
        }
        return s.substring(0, i)
    }

    /**
     * ラベルを、その形式で使える形に整える。"ascii-upper" (FAT) = 大文字の ASCII だけ・[FormatCap.labelMax] 文字まで、"utf8" (ext) = [FormatCap.labelMax] バイトまで、
     * それ以外 (exFAT / NTFS の UTF-16) = [FormatCap.labelMax] 文字まで。使えない文字の細かい規則は、計画 (ネイティブ) が断る。[cap] が null なら変えない。
     */
    fun sanitizeLabel(s: String, cap: FormatCap?): String {
        if (cap == null || cap.labelMax <= 0) return s
        return when (cap.labelCharset) {
            "ascii-upper" -> s.uppercase().filter { it.code in 0x20..0x7E }.take(cap.labelMax)
            "utf8" -> takeUtf8Bytes(s, cap.labelMax)
            else -> s.take(cap.labelMax)
        }
    }

    /**
     * ext の機能の指定 (mke2fs 風)。[features] は [FormatCap.features] ("name" = 既定で有効、"^name" = 既定で無効)、[toggled] = 既定から切り替えた機能の名前。
     * 既定のままなら空。
     */
    fun featureSpec(features: List<String>, toggled: Set<String>): String {
        val out = ArrayList<String>()
        for (f in features) {
            val name = f.removePrefix("^")
            if (name !in toggled) continue
            out.add(if (f.startsWith("^")) name else "^$name")
        }
        return out.joinToString(",")
    }
}
