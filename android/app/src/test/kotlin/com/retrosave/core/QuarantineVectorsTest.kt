package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Exécute en JVM les mêmes vecteurs de quarantaine (AD-30) que Python et C++.
 *
 * Le casse-boucle est ce qui empêche une seule unité empoisonnée de bloquer
 * *toute* la synchronisation. Trois implémentations qui n'en tirent pas la même
 * conclusion produiraient trois comportements sur le même incident — et c'est
 * précisément le genre de divergence que les vecteurs partagés existent pour
 * rendre impossible.
 */
class QuarantineVectorsTest {
    private fun cases() =
        checkNotNull(javaClass.getResourceAsStream("/quarantine_decision.json")) {
            "shared quarantine vector is missing"
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
    fun `shared quarantine vectors`() {
        cases().forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val actual =
                quarantineDecision(
                    consecutiveFailures = input.getValue("consecutive_failures").jsonPrimitive.int,
                    lastFailureHead = input.getValue("last_failure_head").jsonPrimitive.intOrNull,
                    headVersion = input.getValue("head_version").jsonPrimitive.intOrNull,
                )
            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                actual,
            )
        }
    }
}
