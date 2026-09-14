package com.retrosave.core.sync

import com.retrosave.core.model.LocalUnit
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Exécute en JVM les mêmes vecteurs « début de session » que le client Python.
 *
 * `unitsBehindCloud` existe des deux côtés : sans ce fichier commun, les deux
 * moitiés pourraient diverger sur le genre de cas limite qui produit un
 * avertissement de trop — ou pas d'avertissement du tout.
 */
class CloudAheadVectorsTest {
    @Test
    fun `shared cloud ahead vectors`() {
        val resource =
            checkNotNull(javaClass.getResourceAsStream("/cloud_ahead_basic.json")) {
                "shared cloud-ahead vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val remoteUnits =
                input.getValue("remote").jsonArray.map { remote ->
                    val unit = remote.jsonObject
                    remoteUnit(
                        emulator = unit.getValue("emulator").jsonPrimitive.content,
                        unitKey = unit.getValue("unit_key").jsonPrimitive.content,
                        headVersion = unit.getValue("head_version").jsonPrimitive.int,
                        state = unit.getValue("state").jsonPrimitive.content,
                    )
                }
            val localUnits =
                input.getValue("local").jsonArray.map { entry ->
                    val unit = entry.jsonObject
                    localUnit(
                        emulator = unit.getValue("emulator").jsonPrimitive.content,
                        unitKey = unit.getValue("unit_key").jsonPrimitive.content,
                        lastSyncedVersion = unit.getValue("last_synced_version").jsonPrimitive.int,
                    )
                }
            val expected = case.getValue("expected").jsonArray.map { it.jsonPrimitive.content }

            val actual =
                unitsBehindCloud(
                    emulator = input.getValue("emulator").jsonPrimitive.content,
                    remoteUnits = remoteUnits,
                    localUnits = localUnits,
                )

            assertEquals(case.getValue("name").jsonPrimitive.content, expected, actual)
        }
    }

    private fun remoteUnit(
        emulator: String,
        unitKey: String,
        headVersion: Int,
        state: String,
    ) = RemoteUnit(
        id = "srv-$unitKey",
        emulator = emulator,
        unitKey = unitKey,
        unitType = "file",
        gameKey = unitKey,
        gameLabel = unitKey,
        headVersion = headVersion,
        state = state,
        head = null,
    )

    private fun localUnit(
        emulator: String,
        unitKey: String,
        lastSyncedVersion: Int,
    ) = LocalUnit(
        localId = 1,
        serverId = "srv-$unitKey",
        emulator = emulator,
        unitKey = unitKey,
        unitType = "file",
        gameKey = unitKey,
        gameLabel = unitKey,
        rootId = "r",
        relPath = unitKey,
        observedQfHash = null,
        observedAt = null,
        stableQfHash = null,
        stableSince = null,
        lastSyncedVersion = lastSyncedVersion,
        lastSyncedContentSha256 = null,
        state = "active",
        openConflictId = null,
        pendingBranchNumber = null,
        pendingBranchContentSha256 = null,
    )
}
