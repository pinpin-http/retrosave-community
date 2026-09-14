package com.retrosave.core.archive

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.Base64

/** Exécute en JVM les mêmes vecteurs de contenu que le client Python. */
class ContentHashVectorsTest {
    @Test
    fun `shared content hash vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/content_hash_basic.json"),
            ) {
                "shared content-hash vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val files =
                input.getValue("files").jsonArray.map { fileElement ->
                    val file = fileElement.jsonObject
                    ContentFile(
                        relPath = file.getValue("rel_path").jsonPrimitive.content,
                        content =
                            Base64
                                .getDecoder()
                                .decode(file.getValue("content_base64").jsonPrimitive.content),
                    )
                }
            val actual =
                contentSha256(
                    unitType = input.getValue("unit_type").jsonPrimitive.content,
                    files = files,
                )

            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                actual,
            )
        }
    }
}
