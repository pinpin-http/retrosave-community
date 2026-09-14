package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.Base64

/**
 * Exécute en JVM les mêmes vecteurs `PARAM.SFO` que le client Python.
 *
 * Sans ce fichier commun, deux parseurs pourraient donner deux titres
 * différents pour le même `PARAM.SFO` selon l'appareil qui découvre l'unité en
 * premier — et le désaccord ne se verrait qu'une fois les deux clients côte à
 * côte, c'est-à-dire trop tard.
 */
class ParamSfoVectorsTest {
    @Test
    fun `shared param sfo vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/param_sfo_basic.json")) {
                "shared PARAM.SFO vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val name = case.getValue("name").jsonPrimitive.content
            val raw = Base64.getDecoder().decode(case.getValue("input_base64").jsonPrimitive.content)
            val parsed = parseParamSfo(raw)
            val expected = case.getValue("expected")

            if (expected == JsonNull) {
                assertNull(name, parsed)
                return@forEach
            }

            assertNotNull(name, parsed)
            val fields = expected.jsonObject
            assertEquals("$name/title", fields.getValue("title").text(), parsed!!.title)
            assertEquals(
                "$name/savedata_title",
                fields.getValue("savedata_title").text(),
                parsed.savedataTitle,
            )
        }
    }

    @Test
    fun `no input can raise`() {
        // M8 §10 : le pire résultat acceptable est « on affiche l'identifiant ».
        // Un SFO du monde réel peut être n'importe quoi ; balayer des troncatures
        // et des octets retournés coûte moins cher que de découvrir la mauvaise
        // combinaison sur l'appareil.
        val resource = checkNotNull(javaClass.getResourceAsStream("/param_sfo_basic.json"))
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }
        val full =
            Base64.getDecoder().decode(
                cases[0]
                    .jsonObject
                    .getValue("input_base64")
                    .jsonPrimitive.content,
            )

        for (cut in 0..full.size) {
            parseParamSfo(full.copyOfRange(0, cut))
        }
        for (flip in full.indices step 3) {
            val mutated = full.copyOf()
            mutated[flip] = (mutated[flip].toInt() xor 0xFF).toByte()
            parseParamSfo(mutated)
        }
    }

    private fun kotlinx.serialization.json.JsonElement.text(): String? = if (this == JsonNull) null else jsonPrimitive.content
}
