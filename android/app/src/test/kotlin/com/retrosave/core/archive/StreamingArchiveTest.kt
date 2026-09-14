package com.retrosave.core.archive

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.security.MessageDigest

/**
 * AD-29 — la règle du tampon constant, prouvée structurellement.
 *
 * Ce test tourne sous `-Xmx64m` via la tâche Gradle `constrainedHeapTest` : le
 * tas ne peut pas contenir l'unité de 200 Mio, donc sa réussite *est* la preuve
 * qu'aucun chemin ne la matérialise. C'est ce que le Thor a démenti le 01/08 en
 * mourant sur 64 Mio dans `KtorSyncApi.transfer`.
 *
 * Sous un tas normal il passe aussi, sans rien prouver : c'est la contrainte
 * qui porte la démonstration, pas les assertions.
 */
class StreamingArchiveTest {
    @get:Rule
    val folder = TemporaryFolder()

    private val unitBytes = 200L * 1024 * 1024
    private val partCount = 8

    private fun writeIncompressible(
        target: File,
        seed: Int,
        size: Long,
    ) {
        // Un contenu compressible viderait le test de sa substance : zstd
        // réduirait l'archive à presque rien et plus aucun volume ne
        // traverserait le compresseur ni le réseau. Chaque tampon est donc
        // tiré à neuf, jamais répété.
        val random = java.util.Random(seed.toLong())
        val buffer = ByteArray(64 * 1024)
        target.outputStream().buffered().use { out ->
            var remaining = size
            while (remaining > 0) {
                random.nextBytes(buffer)
                val chunk = minOf(buffer.size.toLong(), remaining).toInt()
                out.write(buffer, 0, chunk)
                remaining -= chunk
            }
        }
    }

    @Test
    fun `a two hundred megabyte unit round trips without ever being held`() =
        runTest {
            val sources = folder.newFolder("sources")
            val entries =
                (0 until partCount).map { index ->
                    val part = File(sources, "part$index.bin")
                    writeIncompressible(part, index, unitBytes / partCount)
                    ArchiveSource(part.name, part.length()) { part.inputStream() }
                }

            val archive = File(folder.newFolder("out"), "archive.tar.zst")
            val info = createArchiveFrom("dir", entries, archive)

            assertEquals(unitBytes, info.sizeBytes)
            assertEquals(archive.length(), info.archiveBytes)
            // Garde-fou contre un contenu devenu compressible par accident : sans
            // elle, un générateur qui se répète ferait passer le test en écrasant
            // l'archive à quelques kilo-octets, sans plus rien exercer.
            assertTrue(
                "archive de ${info.archiveBytes} octets : le contenu s'est laissé compresser",
                info.archiveBytes > unitBytes / 2,
            )

            val seen = mutableMapOf<String, Long>()
            val digests =
                extractArchiveTo(
                    archive,
                    "dir",
                    info.contentSha256,
                    folder.newFolder("staging"),
                ) { relPath, staged, sha256 ->
                    val digest = MessageDigest.getInstance("SHA-256")
                    var total = 0L
                    staged.inputStream().buffered().use { input ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            val read = input.read(buffer)
                            if (read <= 0) break
                            digest.update(buffer, 0, read)
                            total += read
                        }
                    }
                    assertEquals(sha256, digest.digest().joinToString("") { "%02x".format(it) })
                    seen[relPath] = total
                }

            assertEquals(partCount, digests.size)
            assertEquals(unitBytes, seen.values.sum())
        }

    @Test
    fun `a wrong content hash writes nothing at all`() =
        runTest {
            val sources = folder.newFolder("small")
            val part = File(sources, "a.bin")
            part.writeBytes(ByteArray(1024) { 7 })
            val archive = File(folder.newFolder("small-out"), "archive.tar.zst")
            createArchiveFrom("dir", listOf(ArchiveSource(part.name, part.length()) { part.inputStream() }), archive)

            val touched = mutableListOf<String>()
            val error =
                runCatching {
                    extractArchiveTo(archive, "dir", "0".repeat(64), folder.newFolder("s2")) { relPath, _, _ ->
                        touched += relPath
                    }
                }.exceptionOrNull()

            assertTrue(error is IllegalArgumentException)
            assertTrue("une archive au mauvais contenu a été appliquée", touched.isEmpty())
        }

    @Test
    fun `the streaming and buffered encodings read each other`() =
        runTest {
            val sources = folder.newFolder("interop")
            val files =
                listOf(
                    ContentFile("a/é.dat", "premier".toByteArray()),
                    ContentFile("b.dat", "second".toByteArray()),
                )
            val entries =
                files.map { file ->
                    val path = File(sources, file.relPath.replace("/", "_"))
                    path.writeBytes(file.content)
                    ArchiveSource(file.relPath, path.length()) { path.inputStream() }
                }

            val expected = contentSha256("dir", files)
            val streamed = File(folder.newFolder("streamed"), "a.tar.zst")
            assertEquals(expected, createArchiveFrom("dir", entries, streamed).contentSha256)

            // Flux -> lecteur tamponné.
            val recovered = extractArchive(streamed.readBytes(), "dir", expected)
            assertEquals(files.map { it.relPath }.sorted(), recovered.map { it.relPath }.sorted())

            // Tamponné -> lecteur en flux.
            val buffered = File(folder.newFolder("buffered"), "b.tar.zst")
            buffered.writeBytes(createArchive("dir", files).bytes)
            val seen = mutableListOf<String>()
            extractArchiveTo(buffered, "dir", expected, folder.newFolder("s3")) { relPath, _, _ ->
                seen += relPath
            }
            assertEquals(files.map { it.relPath }.sorted(), seen.sorted())
        }

    @Test
    fun `a source that changes under us invalidates the archive`() =
        runTest {
            // Q26 : le tar écrit la taille dans l'en-tête *avant* de copier le
            // contenu. Un fichier qui bouge pendant la capture rend l'archive
            // structurellement fausse, et aucune relecture ne la rattrapera —
            // le détecteur vit donc dans l'archiveur lui-même.
            val grown =
                ArchiveSource("mouvant.bin", 1000L) {
                    java.io.ByteArrayInputStream(ByteArray(1500) { 1 })
                }
            val grownError =
                runCatching {
                    createArchiveFrom("dir", listOf(grown), File(folder.newFolder("g"), "a.zst"))
                }.exceptionOrNull()
            assertTrue("un fichier qui grossit doit invalider l'archive", grownError != null)

            val shrunk =
                ArchiveSource("mouvant.bin", 1000L) {
                    java.io.ByteArrayInputStream(ByteArray(400) { 1 })
                }
            val shrunkError =
                runCatching {
                    createArchiveFrom("dir", listOf(shrunk), File(folder.newFolder("s"), "a.zst"))
                }.exceptionOrNull()
            assertTrue("un fichier tronqué doit invalider l'archive", shrunkError != null)
        }
}
