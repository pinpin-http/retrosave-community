package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Exécute en JVM les mêmes vecteurs de libellé que le client Python.
 *
 * M8 §10 : même jeu, même titre affiché sur les deux clients. C'est la seule
 * chose qui empêche une bibliothèque de montrer « Pokemon Platinum Version » ici
 * et « Pokemon Platinum Version (USA) » là.
 */
class DisplayNameVectorsTest {
    @Test
    fun `shared display name vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/display_name_basic.json")) {
                "shared display-name vector is missing"
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
                displayName(case.getValue("input").jsonPrimitive.content),
            )
        }
    }

    @Test
    fun `normalize game name is untouched`() {
        // §5 : `normalizeGameName` est une clé d'appariement, pas un libellé.
        // Ce test existe pour qu'une refonte de `displayName` ne déborde pas sur
        // elle par commodité — renommer des `game_key` casserait l'appariement.
        assertEquals(
            "pokemon platinum version",
            normalizeGameName("Pokemon_Platinum_Version_(USA)_[!].sav"),
        )
        assertEquals(
            "Pokemon Platinum Version",
            displayName("Pokemon_Platinum_Version_(USA)_[!].sav"),
        )
    }
}
