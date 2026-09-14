package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Rejoue en JVM les vecteurs de pause et de sélection locales (SYN-06, SYN-07)
 * que le noyau C++ rejoue de son côté.
 *
 * Ce que ces vecteurs protègent : deux implémentations qui ne s'accorderaient
 * pas sur « cet appareil doit-il s'occuper de cette sauvegarde ? » laisseraient
 * un jeu mis en pause sur le PC continuer d'être poussé depuis la console — ou
 * l'inverse, un jeu qu'on croit suivi rester silencieusement de côté.
 */
class LocalSyncModeVectorsTest {
    private fun cases() =
        checkNotNull(javaClass.getResourceAsStream("/local_sync_mode.json")) {
            "shared local sync mode vector is missing"
        }.bufferedReader(Charsets.UTF_8).use { reader ->
            Json.parseToJsonElement(reader.readText()).jsonArray
        }

    @Test
    fun `the shared vector file is not empty`() {
        // Un fichier introuvable rendrait le test suivant vert sans rien
        // vérifier : on refuse ce silence-là explicitement.
        assertTrue(cases().isNotEmpty())
    }

    @Test
    fun `shared local sync mode vectors`() {
        cases().forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val actual =
                localSyncDecision(
                    mode = input.getValue("mode").jsonPrimitive.content,
                    globalPause = input.getValue("global_pause").jsonPrimitive.boolean,
                )
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                actual,
            )
        }
    }
}
