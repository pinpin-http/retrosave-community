package com.retrosave.core.fs

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes vecteurs AD-27 que le client Python. */
class WriteGuardVectorsTest {
    @Test
    fun `shared rename outcome vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/write_rename_outcome.json")) {
                "shared rename-outcome vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val obtainedElement = input.getValue("obtained")
            val obtained =
                if (obtainedElement == JsonNull) null else obtainedElement.jsonPrimitive.content
            val actual =
                renameOutcome(
                    requested = input.getValue("requested").jsonPrimitive.content,
                    obtained = obtained,
                )
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                actual,
            )
        }
    }

    @Test
    fun `shared path guard vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/path_guard_basic.json")) {
                "shared path-guard vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val relPath =
                case
                    .getValue("input")
                    .jsonObject
                    .getValue("rel_path")
                    .jsonPrimitive
                    .content
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                pathGuard(relPath),
            )
        }
    }
}
