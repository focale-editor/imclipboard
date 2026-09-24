package app.focaleeditor.imclipboard

import java.io.ByteArrayInputStream
import java.io.IOException
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/** Checks bounded provider reads independently of Android framework mocks. */
internal class EncodedImageInputTest {
    @Test
    fun preservesBytesAtTheLimit() {
        val bytes = ByteArray(131073) { it.toByte() }
        assertContentEquals(bytes, readEncodedImage(ByteArrayInputStream(bytes), bytes.size))
    }

    @Test
    fun rejectsAnOversizedProviderStream() {
        assertFailsWith<IOException> { readEncodedImage(ByteArrayInputStream(ByteArray(1025)), 1024) }
    }

    @Test
    fun distinguishesPngFromOtherFormatsAndTruncatedHeaders() {
        val signature = byteArrayOf(0x89.toByte(), 80, 78, 71, 13, 10, 26, 10)
        assertTrue(hasPngSignature(signature + ByteArray(16)))
        assertFalse(hasPngSignature(signature))
        assertFalse(hasPngSignature(ByteArray(24)))
    }
}
