package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Rejoue les vecteurs d'adoption de libellé que le serveur et le noyau C++
 * rejouent de leur côté.
 *
 * Trois implémentations qui ne trancheraient pas pareil donneraient une
 * bibliothèque dont les noms changent à chaque passe, selon l'appareil qui a
 * parlé en dernier.
 */
class LabelAdoptionVectorsTest {
    private fun cases() =
        checkNotNull(javaClass.getResourceAsStream("/label_adoption.json")) {
            "shared label adoption vector is missing"
        }.bufferedReader(Charsets.UTF_8).use { reader ->
            Json.parseToJsonElement(reader.readText()).jsonArray
        }

    @Test
    fun `the shared vector file is not empty`() {
        assertTrue(cases().isNotEmpty())
    }

    @Test
    fun `shared label adoption vectors`() {
        cases().forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val actual =
                adoptedLabel(
                    storedLabel = input.getValue("stored_label").jsonPrimitive.content,
                    storedSource = input.getValue("stored_source").jsonPrimitive.content,
                    incomingLabel = input.getValue("incoming_label").jsonPrimitive.content,
                    unitKey = input.getValue("unit_key").jsonPrimitive.content,
                )
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.contentOrNull,
                actual,
            )
        }
    }
}
