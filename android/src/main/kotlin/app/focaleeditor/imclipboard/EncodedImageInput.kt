package app.focaleeditor.imclipboard

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream

/** Reads provider streams with a bound even when their advertised size is wrong. */
internal fun readEncodedImage(stream: InputStream, maximumBytes: Int): ByteArray {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(64 * 1024)
    while (true) {
        val count = stream.read(buffer)
        if (count < 0) break
        if (count == 0) {
            val byte = stream.read()
            if (byte < 0) break
            if (output.size() >= maximumBytes) throw IOException("The clipboard image exceeds the encoded size limit")
            output.write(byte)
        } else {
            if (output.size().toLong() + count > maximumBytes) throw IOException("The clipboard image exceeds the encoded size limit")
            output.write(buffer, 0, count)
        }
    }
    return output.toByteArray()
}

/** Identifies PNG data; pixel validity is checked by BitmapFactory before use. */
internal fun hasPngSignature(bytes: ByteArray): Boolean =
    bytes.size >= 24 &&
        bytes[0] == 0x89.toByte() && bytes[1] == 0x50.toByte() &&
        bytes[2] == 0x4E.toByte() && bytes[3] == 0x47.toByte() &&
        bytes[4] == 0x0D.toByte() && bytes[5] == 0x0A.toByte() &&
        bytes[6] == 0x1A.toByte() && bytes[7] == 0x0A.toByte()
