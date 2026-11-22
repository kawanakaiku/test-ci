package org.usbmem.app

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** [Edition] の JVM 単体試験 (ネイティブ層なし)。個人版のビルドでは常に「全部対応」であること、ネイティブの JSON の読み取り。 */
class EditionTest {
    @Test fun parseFs_readsNativeJson() {
        assertEquals(setOf("f2fs"), Edition.parseFs("""{"edition":"public","fs":["f2fs"]}"""))
        assertEquals(setOf("ext3", "ext4", "fat32"), Edition.parseFs("""{"edition":"personal","fs":["ext3","ext4","fat32"]}"""))
        assertEquals(emptySet<String>(), Edition.parseFs("""{"edition":"public","fs":[]}"""))
        // 公開版の FS (自前の F2FS / Btrfs / XFS。ネイティブの jni_edition.h が返す並び)
        assertEquals(setOf("btrfs", "f2fs", "xfs"), Edition.parseFs("""{"edition":"public","fs":["btrfs","f2fs","xfs"]}"""))
    }

    @Test fun editionName_isKnown() {
        assertTrue(BuildConfig.EDITION == "personal" || BuildConfig.EDITION == "public")
        assertEquals(BuildConfig.EDITION == "public", Edition.isPublic)
    }

    @Test fun personalEdition_supportsEverything_withoutTouchingNative() {
        if (Edition.isPublic) return   // 公開版は実機 / エミュレータの計装でネイティブ層に問い合わせる (JVM ではライブラリを読み込めない)
        assertTrue(Edition.supports("ntfs"))
        assertTrue(Edition.supports("f2fs"))
        assertFalse(Edition.supportedFsLabel().isNotEmpty())
    }

    @Test fun unsupportedCode_isEprotonosupport() {
        assertEquals(93, Edition.UNSUPPORTED_CODE)
    }

    @Test fun kfsdRequiredCode_isEnopkg() {
        assertEquals(65, Edition.KFSD_REQUIRED_CODE)
    }
}
