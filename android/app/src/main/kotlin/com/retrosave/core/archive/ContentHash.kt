package com.retrosave.core.archive

import com.retrosave.core.fingerprint.utf8PathComparator
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets
import java.security.MessageDigest

/** Un fichier relatif à l'unité, utilisé pour son identité de contenu. */
data class ContentFile(
    val relPath: String,
    val content: ByteArray,
)

/** Une entrée de manifeste, une fois son fichier empreinté en flux. */
data class ContentDigest(
    val relPath: String,
    val sizeBytes: Long,
    val sha256Hex: String,
)

/** Calcule exactement le `content_sha256` normatif du §5.4. */
fun contentSha256(
    unitType: String,
    files: List<ContentFile>,
): String {
    validateFiles(files)
    if (unitType == "file") {
        require(files.size == 1) {
            "a file unit must contain exactly one file"
        }
        return sha256(files.single().content).toHex()
    }
    require(unitType == "dir") {
        "unsupported unit type: $unitType"
    }

    return directoryContentSha256(
        files.map { file ->
            ContentDigest(
                relPath = file.relPath,
                sizeBytes = file.content.size.toLong(),
                sha256Hex = sha256(file.content).toHex(),
            )
        },
    )
}

/**
 * Hache le manifeste trié à partir d'empreintes déjà calculées.
 *
 * C'est ce qui rend §5.4 compatible avec AD-29 : l'identité d'une unité se
 * calcule sans jamais tenir un seul de ses fichiers, pourvu qu'on sache son
 * chemin, sa taille et son empreinte.
 */
fun directoryContentSha256(files: List<ContentDigest>): String {
    validateDigests(files)
    val manifest = ByteArrayOutputStream()
    files
        .sortedWith { left, right ->
            utf8PathComparator.compare(left.relPath, right.relPath)
        }.forEach { file ->
            manifest.write(file.relPath.toByteArray(StandardCharsets.UTF_8))
            manifest.write(0)
            manifest.write(file.sizeBytes.toString().toByteArray(StandardCharsets.US_ASCII))
            manifest.write(0)
            manifest.write(file.sha256Hex.toByteArray(StandardCharsets.US_ASCII))
            manifest.write('\n'.code)
        }
    return sha256(manifest.toByteArray()).toHex()
}

private fun validateDigests(files: List<ContentDigest>) {
    validateFiles(files.map { ContentFile(it.relPath, ByteArray(0)) })
    files.forEach { file ->
        require(file.sizeBytes >= 0) { "file size cannot be negative" }
        require(file.sha256Hex.length == 64 && file.sha256Hex.all { it in "0123456789abcdef" }) {
            "file SHA-256 must contain 64 lowercase hexadecimal characters"
        }
    }
}

private fun validateFiles(files: List<ContentFile>) {
    val seen = mutableSetOf<String>()
    files.forEach { file ->
        val components = file.relPath.split("/")
        require(
            file.relPath.isNotEmpty() &&
                !file.relPath.startsWith("/") &&
                "\\" !in file.relPath &&
                components.none { it.isEmpty() || it == "." || it == ".." },
        ) {
            "unsafe relative path: ${file.relPath}"
        }
        require(seen.add(file.relPath)) {
            "duplicate relative path: ${file.relPath}"
        }
    }
}

private fun sha256(content: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(content)

private fun ByteArray.toHex(): String =
    joinToString("") { byte ->
        "%02x".format(byte.toInt() and 0xff)
    }
