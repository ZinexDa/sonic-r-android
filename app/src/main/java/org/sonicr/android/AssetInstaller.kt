package org.sonicr.android

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream

/**
 * Handles discovery, case-insensitive indexing, validation, and installation
 * of retail Sonic R PC assets from a user-selected SAF tree into app internal storage.
 */
object AssetInstaller {

    const val REQUIRED_PROBE_FILE = "GENERAL/SONICR.BIT"

    val REQUIRED_CORE_FILES = listOf(
        "GENERAL/SONICR.BIT",
        "GENERAL/SONICR.RAW",
        "ISLAND/ISLAND_E.BIN",
        "ISLAND/ISLAND01.RAW",
        "BIN/OBJECTS/SONIC/SONIC_H.BIN",
        "BIN/OBJECTS/SONIC/SONIC_H.GRD",
        "AI/AISTUFFI.BIN"
    )

    data class InstallProgress(
        val currentFile: String,
        val filesCopied: Int,
        val totalFiles: Int,
        val percent: Int,
        val bytesCopied: Long
    )

    sealed class InstallResult {
        data class Success(val filesCopied: Int, val totalBytes: Long) : InstallResult()
        data class MissingFiles(val missing: List<String>) : InstallResult()
        data class Failure(val errorMessage: String) : InstallResult()
    }

    /**
     * Checks if the required retail assets are already installed in app private storage.
     */
    fun isAssetsInstalled(context: Context): Boolean {
        val probeFile = File(context.filesDir, REQUIRED_PROBE_FILE)
        return probeFile.exists() && probeFile.length() > 0
    }

    /**
     * Normalizes a relative path by converting backslashes to forward slashes,
     * removing redundant slashes, and upper-casing all path components.
     */
    fun normalizeRelativePath(path: String): String {
        return path.replace('\\', '/')
            .split('/')
            .filter { it.isNotBlank() && it != "." }
            .joinToString("/") { it.uppercase() }
    }

    /**
     * Finds the root prefix within indexed files where REQUIRED_PROBE_FILE is located.
     * Returns "" if probe is at the root, or "SUBDIR/" if nested, or null if not found.
     */
    fun findGameRootPrefix(indexedKeys: Set<String>): String? {
        if (indexedKeys.contains(REQUIRED_PROBE_FILE)) {
            return ""
        }
        for (key in indexedKeys) {
            if (key.endsWith("/$REQUIRED_PROBE_FILE")) {
                return key.substring(0, key.length - REQUIRED_PROBE_FILE.length)
            }
        }
        return null
    }

    /**
     * Validates that the provided keys (normalized relative to the game root) contain
     * all required core files.
     */
    fun validateRequiredFiles(keys: Set<String>): List<String> {
        val missing = mutableListOf<String>()
        for (required in REQUIRED_CORE_FILES) {
            if (!keys.contains(required)) {
                missing.add(required)
            }
        }
        return missing
    }

    /**
     * Recursively traverses a DocumentFile tree and indexes all files into a map
     * of normalized uppercase relative paths -> DocumentFile.
     */
    suspend fun indexDocumentTree(context: Context, rootDoc: DocumentFile): Map<String, DocumentFile> =
        withContext(Dispatchers.IO) {
            val resultMap = mutableMapOf<String, DocumentFile>()
            traverseDirectory(rootDoc, "", resultMap)
            resultMap
        }

    private fun traverseDirectory(
        dir: DocumentFile,
        currentRelPath: String,
        outMap: MutableMap<String, DocumentFile>
    ) {
        val children = dir.listFiles()
        for (child in children) {
            val name = child.name ?: continue
            val childRelPath = if (currentRelPath.isEmpty()) name else "$currentRelPath/$name"
            if (child.isDirectory) {
                traverseDirectory(child, childRelPath, outMap)
            } else if (child.isFile) {
                val normalizedKey = normalizeRelativePath(childRelPath)
                outMap[normalizedKey] = child
            }
        }
    }

    /**
     * Executes the full asset installation pipeline:
     * 1. Persists SAF permission grant.
     * 2. Indexes the source DocumentTree.
     * 3. Detects game root folder and validates required files.
     * 4. Copies all files into context.filesDir with uppercase paths.
     */
    suspend fun installFromTreeUri(
        context: Context,
        treeUri: Uri,
        onProgress: (InstallProgress) -> Unit
    ): InstallResult = withContext(Dispatchers.IO) {
        try {
            // Persist URI read permission across app restarts
            try {
                context.contentResolver.takePersistableUriPermission(
                    treeUri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (_: Exception) {
                // Ignore if not supported by provider or already held
            }

            val rootDoc = DocumentFile.fromTreeUri(context, treeUri)
                ?: return@withContext InstallResult.Failure("Unable to open selected folder.")

            val indexedFiles = indexDocumentTree(context, rootDoc)
            if (indexedFiles.isEmpty()) {
                return@withContext InstallResult.Failure("The selected folder is empty.")
            }

            val rootPrefix = findGameRootPrefix(indexedFiles.keys)
                ?: return@withContext InstallResult.MissingFiles(listOf(REQUIRED_PROBE_FILE))

            // Remap keys relative to the detected game root
            val gameFiles = mutableMapOf<String, DocumentFile>()
            for ((key, docFile) in indexedFiles) {
                if (key.startsWith(rootPrefix)) {
                    val relativeKey = key.substring(rootPrefix.length)
                    if (relativeKey.isNotEmpty()) {
                        gameFiles[relativeKey] = docFile
                    }
                }
            }

            val missing = validateRequiredFiles(gameFiles.keys)
            if (missing.isNotEmpty()) {
                return@withContext InstallResult.MissingFiles(missing)
            }

            val targetBaseDir = context.filesDir
            val totalFiles = gameFiles.size
            var filesCopied = 0
            var totalBytesCopied = 0L

            val buffer = ByteArray(64 * 1024)

            for ((relUpperPath, docFile) in gameFiles) {
                val targetFile = File(targetBaseDir, relUpperPath)
                targetFile.parentFile?.mkdirs()

                val inputStream: InputStream? = context.contentResolver.openInputStream(docFile.uri)
                if (inputStream == null) {
                    return@withContext InstallResult.Failure("Failed to read file: $relUpperPath")
                }

                inputStream.use { input ->
                    FileOutputStream(targetFile).use { output ->
                        var bytesRead: Int
                        while (input.read(buffer).also { bytesRead = it } != -1) {
                            output.write(buffer, 0, bytesRead)
                            totalBytesCopied += bytesRead
                        }
                    }
                }

                filesCopied++
                val percent = ((filesCopied.toDouble() / totalFiles.toDouble()) * 100).toInt()
                withContext(Dispatchers.Main) {
                    onProgress(
                        InstallProgress(
                            currentFile = relUpperPath,
                            filesCopied = filesCopied,
                            totalFiles = totalFiles,
                            percent = percent,
                            bytesCopied = totalBytesCopied
                        )
                    )
                }
            }

            ensureDirectoriesExist(context)
            InstallResult.Success(filesCopied, totalBytesCopied)
        } catch (e: Exception) {
            InstallResult.Failure(e.localizedMessage ?: e.toString())
        }
    }

    /**
     * Ensures writable save, ghost, and demo directories exist in app storage.
     */
    fun ensureDirectoriesExist(context: Context) {
        File(context.filesDir, "SAVE").mkdirs()
        File(context.filesDir, "GHOST").mkdirs()
        File(context.filesDir, "DEMOS").mkdirs()
    }
}
