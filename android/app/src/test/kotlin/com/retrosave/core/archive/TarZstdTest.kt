package com.retrosave.core.archive

import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream

private const val CONTENT_SHA256 =
    "9c6ddeca1cc0a4ffdf1845805938e42642750450f036cf6be8d1c83c468853f8"

/** Vérifie le déterminisme Kotlin et le round-trip local obligatoire. */
class TarZstdTest {
    private val files =
        listOf(
            ContentFile("b.dat", "beta".toByteArray()),
            ContentFile("a/é.dat", "été".toByteArray()),
        )

    @Test
    fun `archive is deterministic and round trips`() {
        val first = createArchive("dir", files)
        val second = createArchive("dir", files.reversed())

        assertArrayEquals(first.bytes, second.bytes)
        assertEquals(first.contentSha256, CONTENT_SHA256)
        assertEquals(first.sizeBytes, 9)
        assertEquals(first.archiveBytes, first.bytes.size.toLong())

        val extracted = extractArchive(first.bytes, "dir", CONTENT_SHA256)
        assertEquals(listOf("a/é.dat", "b.dat"), extracted.map { it.relPath })
        assertArrayEquals("été".toByteArray(), extracted[0].content)
        assertArrayEquals("beta".toByteArray(), extracted[1].content)
    }

    @Test
    fun `tar metadata is normalized`() {
        // Q25 : le producteur ne déclare plus la taille décompressée, donc la
        // décompression passe par le flux — `Zstd.decompress` exige ce champ.
        val rawTar =
            com.github.luben.zstd
                .ZstdInputStream(ByteArrayInputStream(createArchive("dir", files).bytes))
                .use { it.readBytes() }
        assertEquals(0, rawTar.size % 10_240)
        assertTrue(rawTar.takeLast(1_024).all { it == 0.toByte() })

        TarArchiveInputStream(ByteArrayInputStream(rawTar)).use { tar ->
            while (true) {
                val entry = tar.nextEntry ?: break
                assertTrue(entry.isFile)
                assertEquals(0, entry.modTime.time)
                assertEquals(0, entry.userId)
                assertEquals(0, entry.groupId)
                assertEquals("", entry.userName)
                assertEquals("", entry.groupName)
                assertEquals(0b110100100, entry.mode)
                assertTrue(
                    entry.extraPaxHeaders.keys.none {
                        it == "atime" || it == "ctime" || it == "mtime"
                    },
                )
            }
        }
    }

    @Test
    fun `readers stay tolerant of the legacy content size encoding`() {
        // Q25 : fixture de compatibilité, pas de référence. Des objets portant
        // le champ existent déjà côté serveur et les versions sont immuables ;
        // retirer ce test ferait silencieusement perdre l'accès à l'historique.
        val rawTar =
            com.github.luben.zstd
                .ZstdInputStream(ByteArrayInputStream(createArchive("dir", files).bytes))
                .use { it.readBytes() }
        val legacy =
            com.github.luben.zstd.ZstdCompressCtx().use { compressor ->
                compressor
                    .setLevel(10)
                    .setChecksum(true)
                    .setContentSize(true)
                    .compress(rawTar)
            }

        assertTrue(
            "la fixture doit vraiment porter le champ que l'on n'émet plus",
            com.github.luben.zstd.Zstd
                .getFrameContentSize(legacy) > 0,
        )
        val recovered = extractArchive(legacy, "dir", contentSha256("dir", files))
        assertEquals(files.size, recovered.size)
    }
}
