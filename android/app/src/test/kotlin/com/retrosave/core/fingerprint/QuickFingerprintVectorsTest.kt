package com.retrosave.core.fingerprint

import com.retrosave.core.model.NodeMeta
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes vecteurs QF que le client Python. */
class QuickFingerprintVectorsTest {
    @Test
    fun `shared quick fingerprint vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/fingerprint_basic.json"),
            ) {
                "shared fingerprint vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val nodes =
                case.getValue("input").jsonArray.map { nodeElement ->
                    val node = nodeElement.jsonObject
                    NodeMeta(
                        relPath = node.getValue("rel_path").jsonPrimitive.content,
                        sizeBytes =
                            node
                                .getValue("size_bytes")
                                .jsonPrimitive
                                .content
                                .toLong(),
                        mtimeMs =
                            node
                                .getValue("mtime_ms")
                                .jsonPrimitive
                                .content
                                .toLong(),
                    )
                }
            val expected = case.getValue("expected").jsonObject

            assertArrayEquals(
                case.getValue("name").jsonPrimitive.content,
                expected
                    .getValue("canonical_hex")
                    .jsonPrimitive
                    .content
                    .hexToByteArray(),
                canonicalQuickFingerprint(nodes),
            )
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                expected.getValue("qf_hash").jsonPrimitive.content,
                quickFingerprintHash(nodes),
            )
        }
    }

    @Test
    fun `null mtime is serialized as zero`() {
        assertArrayEquals(
            canonicalQuickFingerprint(listOf(NodeMeta("save.dat", 4, 0))),
            canonicalQuickFingerprint(listOf(NodeMeta("save.dat", 4, null))),
        )
    }
}
