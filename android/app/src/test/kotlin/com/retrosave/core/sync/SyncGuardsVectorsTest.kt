package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

class SyncGuardsVectorsTest {
    private val vectors: JsonObject by lazy {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/sync_guards.json")) {
                "shared sync-guards vector is missing"
            }
        resource.bufferedReader(Charsets.UTF_8).use {
            Json.parseToJsonElement(it.readText()).jsonObject
        }
    }

    @Test
    fun `shared stability guard vectors`() {
        vectors.getValue("stability").jsonArray.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val actual =
                isStable(
                    state = input.text("state"),
                    observedQfHash = input.text("observed_qf_hash"),
                    stableQfHash = input.text("stable_qf_hash"),
                    stableSinceMs = input.long("stable_since_ms"),
                    nowMs = input.long("now_ms"),
                    stabilizationMs = input.long("stabilization_ms"),
                    emulatorRunning = input.boolean("emulator_running"),
                )
            assertEquals(case.text("name"), case.boolean("expected"), actual)
        }
    }

    @Test
    fun `shared apply emulator guard vectors`() {
        vectors.getValue("apply_emulator_guard").jsonArray.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            assertEquals(
                case.text("name"),
                case.boolean("expected"),
                mayApplyWithEmulator(input.boolean("emulator_running")),
            )
        }
    }

    @Test
    fun `shared missing pause vectors`() {
        vectors.getValue("missing_pause").jsonArray.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val actual =
                shouldPauseForMissing(
                    missingCount = input.int("missing_count"),
                    threshold = input.int("threshold"),
                )
            assertEquals(case.text("name"), case.boolean("expected"), actual)
        }
    }

    @Test
    fun `shared duplicate quarantine vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/sync_decision_duplicate.json")) {
                "shared duplicate-quarantine vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use {
                Json.parseToJsonElement(it.readText()).jsonArray
            }
        cases.forEach { element ->
            val case = element.jsonObject
            val relPaths =
                case
                    .getValue("input")
                    .jsonObject
                    .getValue("rel_paths")
                    .jsonArray
                    .map { it.jsonPrimitive.content }
            assertEquals(case.text("name"), case.text("expected"), quarantineState(relPaths))
        }
    }

    private fun JsonObject.text(key: String): String = getValue(key).jsonPrimitive.content

    private fun JsonObject.long(key: String): Long = text(key).toLong()

    private fun JsonObject.int(key: String): Int = text(key).toInt()

    private fun JsonObject.boolean(key: String): Boolean = text(key).toBoolean()
}
