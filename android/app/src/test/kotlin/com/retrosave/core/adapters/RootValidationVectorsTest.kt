package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM exactement les mêmes cas JSON que le client Python. */
class RootValidationVectorsTest {
    @Test
    fun `shared azahar root validation vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/root_validate_azahar_markers.json"),
            ) {
                "shared root validation vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val presentMarkers =
                input.getValue("present_markers").jsonArray.map {
                    it.jsonPrimitive.content
                }
            val markersPrimary =
                input.getValue("markers_primary").jsonArray.map {
                    it.jsonPrimitive.content
                }
            val markersSecondary =
                input.getValue("markers_secondary").jsonArray.map {
                    it.jsonPrimitive.content
                }

            val actual =
                isValidRoot(
                    presentMarkers = presentMarkers,
                    markersPrimary = markersPrimary,
                    markersSecondary = markersSecondary,
                    markersSecondaryMin =
                        input.getValue("markers_secondary_min").jsonPrimitive.int,
                )

            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.boolean,
                actual,
            )
        }
    }
}
