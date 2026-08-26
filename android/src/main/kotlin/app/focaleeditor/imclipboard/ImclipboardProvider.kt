package app.focaleeditor.imclipboard

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException

/** Exposes the current cached PNG to applications receiving the clipboard URI. */
class ImclipboardProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    override fun getType(uri: Uri): String? = resolveFile(uri)?.let { "image/png" }

    override fun openFile(
        uri: Uri,
        mode: String,
    ): ParcelFileDescriptor {
        if (mode != "r") {
            throw FileNotFoundException("Clipboard images are read-only")
        }
        val file = resolveFile(uri) ?: throw FileNotFoundException("Unknown clipboard image")
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?,
    ): Cursor? {
        val file = resolveFile(uri) ?: return null
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        val cursor = MatrixCursor(columns)
        val row = cursor.newRow()
        columns.forEach { column ->
            when (column) {
                OpenableColumns.DISPLAY_NAME -> row.add(file.name)
                OpenableColumns.SIZE -> row.add(file.length())
                else -> row.add(null)
            }
        }
        return cursor
    }

    override fun insert(
        uri: Uri,
        values: ContentValues?,
    ): Uri? = null

    override fun delete(
        uri: Uri,
        selection: String?,
        selectionArgs: Array<out String>?,
    ): Int = 0

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?,
    ): Int = 0

    private fun resolveFile(uri: Uri): File? {
        val providerContext = context ?: return null
        val segments = uri.pathSegments
        if (segments.size != 2 || segments[0] != "image" || !FILE_NAME.matches(segments[1])) {
            return null
        }
        val file = File(File(providerContext.cacheDir, CACHE_DIRECTORY), segments[1])
        return file.takeIf { it.isFile }
    }

    private companion object {
        val FILE_NAME = Regex("^[0-9a-fA-F-]+\\.png$")
        const val CACHE_DIRECTORY = "imclipboard"
    }
}
