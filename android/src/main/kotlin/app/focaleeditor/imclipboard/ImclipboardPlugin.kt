package app.focaleeditor.imclipboard

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.UUID

/** Reads and writes clipboard images through Android content URIs. */
class ImclipboardPlugin :
    FlutterPlugin,
    MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel
    private var applicationContext: Context? = null

    override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        applicationContext = flutterPluginBinding.applicationContext
        channel = MethodChannel(flutterPluginBinding.binaryMessenger, CHANNEL_NAME)
        channel.setMethodCallHandler(this)
    }

    override fun onMethodCall(
        call: MethodCall,
        result: MethodChannel.Result,
    ) {
        if (call.method == "isSupported") {
            result.success(true)
            return
        }
        val context = applicationContext
        if (context == null) {
            result.error("unavailable", "The Android plugin is detached from the Flutter engine", null)
            return
        }

        try {
            when (call.method) {
                "readFiles" -> result.success(emptyList<String>())
                "readImageInfo" -> result.success(readClipboardImage(context, includePng = false)?.toMap())
                "readImage" -> result.success(readClipboardImage(context, includePng = true)?.toMap())
                "writeImage" -> {
                    writeClipboardImage(context, call.arguments)
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        } catch (exception: Exception) {
            result.error("clipboard_failed", exception.message ?: "Android clipboard operation failed", null)
        }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        applicationContext = null
    }

    private fun readClipboardImage(
        context: Context,
        includePng: Boolean,
    ): ClipboardImage? {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val description = clipboard.primaryClipDescription ?: return null
        if (!description.hasMimeType("image/*")) {
            return null
        }
        val clip = clipboard.primaryClip ?: return null
        var lastFailure: Exception? = null
        for (index in 0 until clip.itemCount) {
            val uri = clip.getItemAt(index).uri ?: continue
            try {
                val mimeType = context.contentResolver.getType(uri)
                if (mimeType != null && !mimeType.startsWith("image/")) {
                    continue
                }
                return readUriImage(context, uri, includePng)
            } catch (exception: Exception) {
                lastFailure = exception
            }
        }
        if (lastFailure != null) {
            throw lastFailure
        }
        return null
    }

    private fun readUriImage(
        context: Context,
        uri: Uri,
        includePng: Boolean,
    ): ClipboardImage {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        context.contentResolver.openInputStream(uri)?.use { stream ->
            BitmapFactory.decodeStream(stream, null, bounds)
        } ?: throw IOException("Android could not open the clipboard image")
        validateDimensions(bounds.outWidth, bounds.outHeight)

        val token = tokenFromUri(context, uri)
        if (!includePng) {
            return ClipboardImage(bounds.outWidth, bounds.outHeight, token, null)
        }

        val bitmap = context.contentResolver.openInputStream(uri)?.use { stream ->
            BitmapFactory.decodeStream(stream)
        } ?: throw IOException("Android could not decode the clipboard image")
        try {
            val output = LimitedByteArrayOutputStream(MAXIMUM_ENCODED_BYTES)
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)) {
                throw IOException("Android could not encode the clipboard image as PNG")
            }
            return ClipboardImage(bitmap.width, bitmap.height, token, output.toByteArray())
        } finally {
            bitmap.recycle()
        }
    }

    private fun writeClipboardImage(
        context: Context,
        rawArguments: Any?,
    ) {
        val arguments = rawArguments as? Map<*, *> ?: throw IllegalArgumentException("Expected clipboard arguments")
        val bytes = arguments["bytes"] as? ByteArray ?: throw IllegalArgumentException("Missing PNG bytes")
        val token = arguments["token"] as? String
        if (bytes.isEmpty() || bytes.size > MAXIMUM_ENCODED_BYTES || !hasPngSignature(bytes)) {
            throw IllegalArgumentException("Expected a PNG smaller than 512 MiB")
        }
        if (token != null && (token.isEmpty() || token.indexOf('\u0000') >= 0 || token.toByteArray().size > MAXIMUM_TOKEN_BYTES)) {
            throw IllegalArgumentException("The clipboard token is invalid")
        }

        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        validateDimensions(bounds.outWidth, bounds.outHeight)

        val directory = File(context.cacheDir, CACHE_DIRECTORY)
        if (!directory.exists() && !directory.mkdirs()) {
            throw IOException("Android could not create the clipboard cache")
        }
        val file = File(directory, "${UUID.randomUUID()}.png")
        file.writeBytes(bytes)

        val authority = context.packageName + PROVIDER_AUTHORITY_SUFFIX
        val builder = Uri.Builder().scheme("content").authority(authority).appendPath("image").appendPath(file.name)
        if (token != null) {
            builder.appendQueryParameter("token", token)
        }
        val uri = builder.build()
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        try {
            clipboard.setPrimaryClip(ClipData.newUri(context.contentResolver, "Image", uri))
        } catch (exception: Exception) {
            file.delete()
            throw exception
        }
        directory.listFiles()?.forEach { candidate ->
            if (candidate != file) {
                candidate.delete()
            }
        }
    }

    private fun tokenFromUri(
        context: Context,
        uri: Uri,
    ): String? {
        if (uri.authority != context.packageName + PROVIDER_AUTHORITY_SUFFIX) {
            return null
        }
        val token = uri.getQueryParameter("token") ?: return null
        return token.takeIf { it.isNotEmpty() && it.indexOf('\u0000') < 0 && it.toByteArray().size <= MAXIMUM_TOKEN_BYTES }
    }

    private fun validateDimensions(
        width: Int,
        height: Int,
    ) {
        if (width < 1 || height < 1 || width.toLong() * height.toLong() > MAXIMUM_PIXEL_COUNT) {
            throw IllegalArgumentException("The clipboard image dimensions are invalid or too large")
        }
    }

    private fun hasPngSignature(bytes: ByteArray): Boolean =
        bytes.size >= 24 &&
            bytes[0] == 0x89.toByte() &&
            bytes[1] == 0x50.toByte() &&
            bytes[2] == 0x4E.toByte() &&
            bytes[3] == 0x47.toByte() &&
            bytes[4] == 0x0D.toByte() &&
            bytes[5] == 0x0A.toByte() &&
            bytes[6] == 0x1A.toByte() &&
            bytes[7] == 0x0A.toByte()

    private data class ClipboardImage(
        val width: Int,
        val height: Int,
        val token: String?,
        val pngBytes: ByteArray?,
    ) {
        fun toMap(): Map<String, Any> =
            buildMap {
                put("width", width)
                put("height", height)
                token?.let { put("token", it) }
                pngBytes?.let { put("bytes", it) }
            }
    }

    private class LimitedByteArrayOutputStream(
        private val maximumBytes: Int,
    ) : ByteArrayOutputStream() {
        override fun write(value: Int) {
            ensureCapacityFor(1)
            super.write(value)
        }

        override fun write(
            buffer: ByteArray,
            offset: Int,
            length: Int,
        ) {
            ensureCapacityFor(length)
            super.write(buffer, offset, length)
        }

        private fun ensureCapacityFor(additionalBytes: Int) {
            if (count.toLong() + additionalBytes > maximumBytes) {
                throw IOException("The encoded clipboard PNG exceeds 512 MiB")
            }
        }
    }

    private companion object {
        const val CHANNEL_NAME = "app.focaleeditor.imclipboard/image_clipboard"
        const val PROVIDER_AUTHORITY_SUFFIX = ".imclipboard"
        const val CACHE_DIRECTORY = "imclipboard"
        const val MAXIMUM_ENCODED_BYTES = 512 * 1024 * 1024
        const val MAXIMUM_TOKEN_BYTES = 1024
        const val MAXIMUM_PIXEL_COUNT = 100_000_000L
    }
}
