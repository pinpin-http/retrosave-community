package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes vecteurs de nom de jeu que le client Python. */
class NameNormalizationVectorsTest {
    @Test
    fun `shared name normalization vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/normalize_name_basic.json"),
            ) {
                "shared name-normalization vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                normalizeGameName(case.getValue("input").jsonPrimitive.content),
            )
        }
    }
}
