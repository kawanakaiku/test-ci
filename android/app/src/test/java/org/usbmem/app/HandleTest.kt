package org.usbmem.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HandleTest {
    @Test
    fun taggedArm64PointerIsAValidHandleEvenThoughNegative() {
        val tagged = 0xb400007b12345678uL.toLong()   // Android 11+ arm64 のヒープポインタ (上位バイト 0xb4)
        assertTrue(tagged < 0)
        assertTrue(Handle.valid(tagged))
        assertFalse(Handle.isError(tagged))
    }

    @Test
    fun plainPointerAndErrorCodes() {
        assertTrue(Handle.valid(0x7f12345678L))
        assertFalse(Handle.valid(0))        // 無し / 閉じた
        assertTrue(Handle.isError(-5)); assertTrue(Handle.isError(-22)); assertTrue(Handle.isError(-4095)); assertTrue(Handle.isError(0))
        assertFalse(Handle.valid(-5))
    }
}
