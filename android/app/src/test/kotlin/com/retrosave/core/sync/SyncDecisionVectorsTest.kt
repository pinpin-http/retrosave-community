package com.retrosave.core.sync

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes chemins Q5-bis que le client Python. */
class SyncDecisionVectorsTest {
    @Test
    fun `shared pending branch decision vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/sync_decision_pending_branch.json"),
            ) {
                "shared pending-branch vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val pendingBranch = input.getValue("pending_branch").jsonObject
            val actual =
                pendingBranchDecision(
                    openConflict =
                        input
                            .getValue("open_conflict")
                            .jsonPrimitive
                            .content
                            .toBoolean(),
                    localContentSha256 =
                        input.getValue("local_content_sha256").jsonPrimitive.content,
                    pendingBranchContentSha256 =
                        pendingBranch.getValue("content_sha256").jsonPrimitive.content,
                )

            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                case.getValue("expected").jsonPrimitive.content,
                actual,
            )
        }
    }
}
