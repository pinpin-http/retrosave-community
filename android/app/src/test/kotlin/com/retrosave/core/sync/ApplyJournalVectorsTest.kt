package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes vecteurs AD-27 de reprise que le client Python. */
class ApplyJournalVectorsTest {
    @Test
    fun `shared apply journal decision vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/apply_journal_decision.json")) {
                "shared apply-journal vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val journalElement = input.getValue("journal")
            val journal =
                if (journalElement == JsonNull) {
                    ApplyJournal.Absent
                } else {
                    // `malformed` ne peut pas être un sha256 de 64 hex, donc
                    // l'encodage plat reste sans ambiguïté (Q19).
                    when (val raw = journalElement.jsonPrimitive.content) {
                        "malformed" -> ApplyJournal.Malformed
                        else -> ApplyJournal.Present(raw)
                    }
                }
            val localElement = input.getValue("local_content_sha256")
            val local =
                if (localElement == JsonNull) null else localElement.jsonPrimitive.content

            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                applyJournalDecision(journal, local),
            )
        }
    }

    @Test
    fun `reader maps three empty columns to absent`() {
        assertEquals(ApplyJournal.Absent, readApplyJournal(null, null, null))
    }

    @Test
    fun `reader maps three filled columns to present`() {
        assertEquals(
            ApplyJournal.Present("a".repeat(64)),
            readApplyJournal(4, "a".repeat(64), 1_700_000_000L),
        )
    }

    @Test
    fun `reader maps any partial row to malformed`() {
        val partials =
            listOf(
                Triple(4, null, 1_700_000_000L),
                Triple(null, "a".repeat(64), 1_700_000_000L),
                Triple(4, "a".repeat(64), null),
                Triple(4, null, null),
            )
        partials.forEach { (version, content, started) ->
            assertEquals(
                "partial row $version/$content/$started",
                ApplyJournal.Malformed,
                readApplyJournal(version, content, started),
            )
        }
    }
}
