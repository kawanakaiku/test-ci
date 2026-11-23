// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.usbmem.app.ui.FormatCap
import org.usbmem.app.ui.FormatChoices

class FormatChoicesTest {
    private fun cap(max: Int, charset: String) = FormatCap(
        "x", true, false, 0L, 0L, emptyList(), emptyList(), 0, max, charset, emptyList(), true, "", "",
    )

    @Test fun autoFat_picksBySize() {
        val mib = 1L shl 20
        val all: (String) -> Boolean = { true }
        assertEquals("fat12", FormatChoices.autoFat(2 * mib, all))
        assertEquals("fat16", FormatChoices.autoFat(64 * mib, all))
        assertEquals("fat32", FormatChoices.autoFat(600 * mib, all))
        assertEquals("fat32", FormatChoices.autoFat(64L * 1024 * mib, all))
    }

    @Test fun autoFat_fallsBackWhenFirstChoiceCannotBeMade() {
        val mib = 1L shl 20
        assertEquals("fat16", FormatChoices.autoFat(600 * mib) { it != "fat32" })
        assertEquals("fat32", FormatChoices.autoFat(4 * mib) { it == "fat32" })
        assertNull(FormatChoices.autoFat(4 * mib) { false })
    }

    @Test fun sanitizeLabel_perCharset() {
        assertEquals("ABC 123", FormatChoices.sanitizeLabel("abc 123", cap(11, "ascii-upper")))
        assertEquals("ABCDEFGHIJK", FormatChoices.sanitizeLabel("abcdefghijklmnop", cap(11, "ascii-upper")))
        assertEquals("AB", FormatChoices.sanitizeLabel("aあb", cap(11, "ascii-upper")))
        // UTF-8 は 16 バイトまで (U+3042 は 3 バイトなので 5 文字 = 15 バイト)
        assertEquals("あああああ", FormatChoices.sanitizeLabel("あああああああ", cap(16, "utf8")))
        assertEquals("ああ", FormatChoices.sanitizeLabel("ああ", cap(16, "utf8")))
        assertEquals("abcde", FormatChoices.sanitizeLabel("abcdefghij", cap(5, "utf16")))
        assertEquals("keep", FormatChoices.sanitizeLabel("keep", null))
    }

    @Test fun featureSpec_onlyTheDifferencesFromDefault() {
        val feats = listOf("has_journal", "^bigalloc", "resize_inode")
        assertEquals("", FormatChoices.featureSpec(feats, emptySet()))
        assertEquals("bigalloc,^resize_inode", FormatChoices.featureSpec(feats, setOf("resize_inode", "bigalloc")))
        assertEquals("^has_journal", FormatChoices.featureSpec(feats, setOf("has_journal", "unknown")))
    }
}
