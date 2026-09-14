package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes vecteurs de placement melonDS que le client Python. */
class RemotePlacementVectorsTest {
    @Test
    fun `shared melonDS placement vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/melonds_placement_basic.json")) {
                "shared melonDS placement vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val expectedElement = case.getValue("expected")
            val expected =
                if (expectedElement == JsonNull) null else expectedElement.jsonPrimitive.content
            val actual =
                melonDsRomDirectory(
                    unitKey = input.getValue("unit_key").jsonPrimitive.content,
                    romRelPaths =
                        input.getValue("rom_rel_paths").jsonArray.map {
                            it.jsonPrimitive.content
                        },
                )

            assertEquals(case.getValue("name").jsonPrimitive.content, expected, actual)
        }
    }
}
