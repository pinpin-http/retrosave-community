package com.retrosave.core.archive

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.nio.file.Files
import java.nio.file.Path
import java.util.Base64

/** Point d'échange JVM du test CI Python↔Kotlin, sans fixture binaire commitée. */
class ArchiveInteropTest {
    @Test
    fun `python archive extracts and kotlin archive is exported`() {
        val pythonArchive = System.getenv("RETROSAVE_PYTHON_ARCHIVE")
        val kotlinArchive = System.getenv("RETROSAVE_KOTLIN_ARCHIVE")
        assumeTrue(
            "cross-language archive paths are supplied by the CI harness",
            pythonArchive != null && kotlinArchive != null,
        )

        val case = sharedDirectoryCase()
        val files =
            case
                .getValue("files")
                .jsonArray
                .map { element ->
                    val file = element.jsonObject
                    ContentFile(
                        relPath = file.getValue("rel_path").jsonPrimitive.content,
                        content =
                            Base64
                                .getDecoder()
                                .decode(file.getValue("content_base64").jsonPrimitive.content),
                    )
                }
        val expected = contentSha256("dir", files)

        val extracted =
            extractArchive(
                Files.readAllBytes(Path.of(checkNotNull(pythonArchive))),
                "dir",
                expected,
            )
        assertEquals(expected, contentSha256("dir", extracted))
        Files.write(
            Path.of(checkNotNull(kotlinArchive)),
            createArchive("dir", files).bytes,
        )
    }

    private fun sharedDirectoryCase() =
        checkNotNull(
            javaClass.getResourceAsStream("/content_hash_basic.json"),
        ).bufferedReader(Charsets.UTF_8).use { reader ->
            Json
                .parseToJsonElement(reader.readText())
                .jsonArray
                .first { element ->
                    element.jsonObject
                        .getValue("input")
                        .jsonObject
                        .getValue("unit_type")
                        .jsonPrimitive
                        .content == "dir" &&
                        element.jsonObject
                            .getValue("input")
                            .jsonObject
                            .getValue("files")
                            .jsonArray
                            .isNotEmpty()
                }.jsonObject
                .getValue("input")
                .jsonObject
        }
}
