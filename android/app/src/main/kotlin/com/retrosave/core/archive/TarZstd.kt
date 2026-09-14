package com.retrosave.core.archive

import com.github.luben.zstd.Zstd
import com.github.luben.zstd.ZstdCompressCtx
import com.github.luben.zstd.ZstdInputStream
import com.github.luben.zstd.ZstdOutputStream
import com.retrosave.core.fingerprint.utf8PathComparator
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.apache.commons.compress.archivers.tar.TarConstants
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FilterOutputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets
import java.security.MessageDigest

private const val TAR_BLOCK_SIZE = 10_240
private const val ZSTD_LEVEL = 10
internal const val BUFFER_SIZE = 64 * 1024

/** Un objet tar.zst complet et ses métadonnées immuables. */
data class ArchiveBlob(
    val bytes: ByteArray,
    val contentSha256: String,
    val archiveSha256: String,
    val sizeBytes: Long,
) {
    val archiveBytes: Long
        get() = bytes.size.toLong()
}

/**
 * Un fichier d'unité à archiver : un nom, une taille, et de quoi l'ouvrir.
 *
 * Q26 : pas un chemin. Un document SAF n'en a pas, et sans ouvreur le client
 * Android devrait recopier chaque unité en transit avant de l'archiver.
 */
data class ArchiveSource(
    val relPath: String,
    val sizeBytes: Long,
    val open: suspend () -> java.io.InputStream,
)

/** Mêmes métadonnées qu'`ArchiveBlob`, pour une archive qui vit sur disque. */
data class ArchiveInfo(
    val contentSha256: String,
    val archiveSha256: String,
    val sizeBytes: Long,
    val archiveBytes: Long,
)

/** Flux d'écriture qui empreinte au passage : rien n'est relu pour être haché. */
private class HashingOutputStream(
    inner: OutputStream,
) : FilterOutputStream(inner) {
    private val digest = MessageDigest.getInstance("SHA-256")
    var total: Long = 0
        private set

    override fun write(byte: Int) {
        out.write(byte)
        digest.update(byte.toByte())
        total += 1
    }

    override fun write(
        buffer: ByteArray,
        offset: Int,
        length: Int,
    ) {
        out.write(buffer, offset, length)
        digest.update(buffer, offset, length)
        total += length.toLong()
    }

    fun hex(): String = digest.digest().toHex()
}

/**
 * Fabrique l'archive en flux : ni l'unité ni un seul fichier ne tient en
 * mémoire (AD-29). Chaque source est lue une fois et empreinte au passage.
 *
 * Q22 : `suspend` parce que les sources viennent du `Vfs`, qui l'est. La parité
 * entre les deux langages est sémantique — mêmes vecteurs, même round-trip
 * d'interop — pas syntaxique ; chacun utilise son système d'effets idiomatique.
 */
suspend fun createArchiveFrom(
    unitType: String,
    entries: List<ArchiveSource>,
    dest: File,
): ArchiveInfo {
    val ordered =
        entries.sortedWith { left, right ->
            utf8PathComparator.compare(left.relPath, right.relPath)
        }
    rejectUnsafeNames(ordered.map { it.relPath })
    require(unitType == "file" || unitType == "dir") { "unsupported unit type: $unitType" }
    require(unitType != "file" || ordered.size == 1) {
        "a file unit must contain exactly one file"
    }

    val digests = mutableListOf<ContentDigest>()
    dest.parentFile?.mkdirs()
    var archiveSha256: String
    var archiveBytes: Long
    dest.outputStream().buffered(BUFFER_SIZE).use { raw ->
        val counter = HashingOutputStream(raw)
        ZstdOutputStream(counter, ZSTD_LEVEL).use { compressed ->
            compressed.setChecksum(true)
            TarArchiveOutputStream(
                compressed,
                TarConstants.DEFAULT_BLKSIZE,
                TarConstants.DEFAULT_RCDSIZE,
                StandardCharsets.UTF_8.name(),
            ).use { tar ->
                tar.setLongFileMode(TarArchiveOutputStream.LONGFILE_POSIX)
                tar.setBigNumberMode(TarArchiveOutputStream.BIGNUMBER_POSIX)
                tar.setAddPaxHeadersForNonAsciiNames(true)
                ordered.forEach { entry ->
                    val name = entry.relPath
                    val size = entry.sizeBytes
                    tar.putArchiveEntry(tarEntry(name, size))
                    val digest = MessageDigest.getInstance("SHA-256")
                    var copied = 0L
                    entry.open().use { handle ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val read = handle.read(buffer)
                            if (read <= 0) break
                            tar.write(buffer, 0, read)
                            digest.update(buffer, 0, read)
                            copied += read
                        }
                    }
                    tar.closeArchiveEntry()
                    // Q26 : le tar a déjà écrit la taille dans l'en-tête. Une
                    // divergence veut dire que le fichier a bougé pendant la
                    // capture : archive irrécupérable, on la jette.
                    require(copied == size) { "source changed while archiving: $name" }
                    digests += ContentDigest(name, size, digest.digest().toHex())
                }
                tar.finish()
            }
        }
        archiveSha256 = counter.hex()
        archiveBytes = counter.total
    }

    val content =
        if (unitType == "file") {
            digests.single().sha256Hex
        } else {
            directoryContentSha256(digests)
        }
    return ArchiveInfo(
        contentSha256 = content,
        archiveSha256 = archiveSha256,
        sizeBytes = digests.sumOf { it.sizeBytes },
        archiveBytes = archiveBytes,
    )
}

/**
 * Étale chaque entrée sur disque, tranche sur l'ensemble, et n'appelle `sink`
 * qu'ensuite : une archive au mauvais contenu n'écrit rien du tout.
 *
 * Q22 : `suspend`, sink compris, parce que le sink écrit par le `Vfs`. Un
 * `runBlocking` depuis une coroutine serait un anti-pattern à interblocage, pas
 * seulement un thread bloqué par fichier.
 */
suspend fun extractArchiveTo(
    archive: File,
    unitType: String,
    expectedContentSha256: String,
    staging: File,
    sink: suspend (String, File, String) -> Unit,
): List<ContentDigest> {
    staging.mkdirs()
    val scratch = File(staging, ".rsc-stage-${System.nanoTime()}")
    check(scratch.mkdirs()) { "cannot create staging directory" }
    try {
        val digests = stageEntries(archive, scratch)
        val actual =
            when (unitType) {
                "file" -> {
                    require(digests.size == 1) { "a file unit must contain exactly one file" }
                    digests.single().sha256Hex
                }
                "dir" -> directoryContentSha256(digests)
                else -> throw IllegalArgumentException("unsupported unit type: $unitType")
            }
        require(actual == expectedContentSha256) {
            "archive content SHA-256 does not match"
        }
        digests.forEach { digest ->
            sink(digest.relPath, File(scratch, stagedName(digest.relPath)), digest.sha256Hex)
        }
        return digests
    } finally {
        scratch.deleteRecursively()
    }
}

private fun stageEntries(
    archive: File,
    scratch: File,
): List<ContentDigest> {
    val digests = mutableListOf<ContentDigest>()
    val seen = mutableSetOf<String>()
    archive.inputStream().buffered(BUFFER_SIZE).use { raw ->
        ZstdInputStream(raw).use { decompressed ->
            TarArchiveInputStream(
                decompressed,
                StandardCharsets.UTF_8.name(),
            ).use { tar ->
                while (true) {
                    val entry = tar.nextEntry ?: break
                    validateEntry(entry, seen)
                    val digest = MessageDigest.getInstance("SHA-256")
                    var written = 0L
                    File(scratch, stagedName(entry.name)).outputStream().buffered(BUFFER_SIZE).use { out ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val read = tar.read(buffer)
                            if (read <= 0) break
                            out.write(buffer, 0, read)
                            digest.update(buffer, 0, read)
                            written += read
                        }
                    }
                    require(written == entry.size) { "truncated archive entry: ${entry.name}" }
                    digests += ContentDigest(entry.name, written, digest.digest().toHex())
                }
            }
        }
    }
    return digests
}

/**
 * Aplatit un chemin d'archive en un seul nom de fichier d'étalement : aucun
 * répertoire n'est jamais créé à partir d'un nom que l'archive a choisi.
 */
private fun stagedName(relPath: String): String =
    MessageDigest
        .getInstance("SHA-256")
        .digest(relPath.toByteArray(StandardCharsets.UTF_8))
        .toHex()

private fun tarEntry(
    name: String,
    size: Long,
): TarArchiveEntry =
    TarArchiveEntry(name).apply {
        this.size = size
        setModTime(0L)
        setIds(0, 0)
        setNames("", "")
        mode = 0b110100100
    }

private fun rejectUnsafeNames(names: List<String>) {
    val seen = mutableSetOf<String>()
    names.forEach { name ->
        val components = name.split("/")
        require(
            name.isNotEmpty() &&
                !name.startsWith("/") &&
                "\\" !in name &&
                components.none { it.isEmpty() || it == "." || it == ".." },
        ) {
            "unsafe relative path: $name"
        }
        require(seen.add(name)) { "duplicate relative path: $name" }
    }
}

private fun ByteArray.toHex(): String =
    joinToString("") { byte ->
        val value = byte.toInt() and 0xff
        "0123456789abcdef"[value shr 4].toString() + "0123456789abcdef"[value and 0x0f]
    }

/** Crée un tar PAX déterministe, puis une frame zstd conforme au §5.3. */
fun createArchive(
    unitType: String,
    files: List<ContentFile>,
): ArchiveBlob {
    val contentHash = contentSha256(unitType, files)
    val rawTar = createTar(files)
    val compressed =
        ZstdCompressCtx().use { compressor ->
            compressor
                .setLevel(ZSTD_LEVEL)
                .setChecksum(true)
                .setWorkers(0)
                // Q25 : voir create_archive côté Python — une seule forme émise.
                .setContentSize(false)
                .setDictID(false)
                .compress(rawTar)
        }
    return ArchiveBlob(
        bytes = compressed,
        contentSha256 = contentHash,
        archiveSha256 = sha256(compressed),
        sizeBytes = files.sumOf { it.content.size.toLong() },
    )
}

/** Extrait sans traversal et décide uniquement sur le hash du contenu extrait. */
fun extractArchive(
    archive: ByteArray,
    unitType: String,
    expectedContentSha256: String,
): List<ContentFile> {
    require(Zstd.findFrameCompressedSize(archive) == archive.size.toLong()) {
        "archive must contain exactly one zstd frame"
    }
    // `Zstd.decompress` réclame que la trame déclare sa taille décompressée,
    // ce qu'un producteur en flux ne peut pas savoir d'avance. Un lecteur n'a
    // pas à exiger un champ d'en-tête facultatif : on passe par le flux, qui
    // accepte les deux formes.
    val rawTar =
        try {
            ZstdInputStream(ByteArrayInputStream(archive)).use { it.readBytes() }
        } catch (error: RuntimeException) {
            throw IllegalArgumentException("archive zstd frame is corrupt", error)
        }
    val files = extractTar(rawTar)
    require(contentSha256(unitType, files) == expectedContentSha256) {
        "archive content SHA-256 does not match"
    }
    return files
}

private fun createTar(files: List<ContentFile>): ByteArray {
    val output = ByteArrayOutputStream()
    TarArchiveOutputStream(
        output,
        TarConstants.DEFAULT_BLKSIZE,
        TarConstants.DEFAULT_RCDSIZE,
        StandardCharsets.UTF_8.name(),
    ).use { tar ->
        tar.setLongFileMode(TarArchiveOutputStream.LONGFILE_POSIX)
        tar.setBigNumberMode(TarArchiveOutputStream.BIGNUMBER_POSIX)
        tar.setAddPaxHeadersForNonAsciiNames(true)
        files
            .sortedWith { left, right ->
                utf8PathComparator.compare(left.relPath, right.relPath)
            }.forEach { file ->
                val entry = TarArchiveEntry(file.relPath)
                entry.size = file.content.size.toLong()
                entry.setModTime(0L)
                entry.setIds(0, 0)
                entry.setNames("", "")
                entry.mode = 0b110100100
                tar.putArchiveEntry(entry)
                tar.write(file.content)
                tar.closeArchiveEntry()
            }
        tar.finish()
    }
    return output.toByteArray().also { rawTar ->
        check(rawTar.size % TAR_BLOCK_SIZE == 0) {
            "Commons Compress did not emit 10 KiB record padding"
        }
        check(rawTar.takeLast(1_024).all { it == 0.toByte() }) {
            "Commons Compress did not emit two tar EOF records"
        }
    }
}

private fun extractTar(rawTar: ByteArray): List<ContentFile> {
    val files = mutableListOf<ContentFile>()
    val seen = mutableSetOf<String>()
    TarArchiveInputStream(
        ByteArrayInputStream(rawTar),
        StandardCharsets.UTF_8.name(),
    ).use { tar ->
        while (true) {
            val entry = tar.nextEntry ?: break
            validateEntry(entry, seen)
            val content = tar.readAllBytes()
            require(content.size.toLong() == entry.size) {
                "truncated archive entry: ${entry.name}"
            }
            files += ContentFile(entry.name, content)
        }
    }
    return files
}

private fun validateEntry(
    entry: TarArchiveEntry,
    seen: MutableSet<String>,
) {
    val components = entry.name.split("/")
    require(
        entry.isFile &&
            entry.name.isNotEmpty() &&
            !entry.name.startsWith("/") &&
            "\\" !in entry.name &&
            components.none { it.isEmpty() || it == "." || it == ".." },
    ) {
        "unsafe or unsupported tar entry: ${entry.name}"
    }
    require(seen.add(entry.name)) {
        "duplicate tar entry: ${entry.name}"
    }
}

private fun sha256(content: ByteArray): String =
    MessageDigest
        .getInstance("SHA-256")
        .digest(content)
        .joinToString("") { byte ->
            "%02x".format(byte.toInt() and 0xff)
        }
