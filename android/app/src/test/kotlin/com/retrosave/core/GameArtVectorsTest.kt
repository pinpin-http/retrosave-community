package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test
import java.util.Base64

/**
 * Exécute en JVM les mêmes vecteurs de vignette que le client Python.
 *
 * Le placeholder est calculé, pas transmis : c'est ce fichier qui garantit que
 * le même jeu porte la même couleur et les mêmes initiales sur les deux
 * appareils. Sans lui, une bibliothèque synchronisée n'aurait pas l'air d'être
 * la même bibliothèque.
 */
class GameArtVectorsTest {
    private fun vectors() =
        checkNotNull(javaClass.getResourceAsStream("/game_art_basic.json")) {
            "shared game-art vector is missing"
        }.bufferedReader(Charsets.UTF_8)
            .use { Json.parseToJsonElement(it.readText()).jsonObject }

    @Test
    fun `shared placeholder hues`() {
        vectors().getValue("hues").jsonArray.forEach { element ->
            val case = element.jsonObject
            val key = case.getValue("game_key").jsonPrimitive.content
            assertEquals(key, case.getValue("expected").jsonPrimitive.int, placeholderHue(key))
        }
    }

    @Test
    fun `shared placeholder initials`() {
        vectors().getValue("initials").jsonArray.forEach { element ->
            val case = element.jsonObject
            val label = case.getValue("label").jsonPrimitive.content
            assertEquals(
                label,
                case.getValue("expected").jsonPrimitive.content,
                placeholderInitials(label),
            )
        }
    }

    @Test
    fun `shared icon validation`() {
        vectors().getValue("icons").jsonArray.forEach { element ->
            val case = element.jsonObject
            val raw = Base64.getDecoder().decode(case.getValue("input_base64").jsonPrimitive.content)
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected_valid").jsonPrimitive.boolean,
                isValidIcon(raw),
            )
        }
    }

    @Test
    fun `no input can raise`() {
        listOf(ByteArray(0), byteArrayOf(-119, 0x50), ByteArray(40)).forEach { isValidIcon(it) }
        listOf("", " ", "!!!", "x".repeat(500)).forEach {
            placeholderInitials(it)
            placeholderHue(it)
        }
    }
}
