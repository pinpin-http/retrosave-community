package com.retrosave.core.adapters

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Test

class AdapterAssetsTest {
    @Test
    fun `generated active adapter assets match their TOML identities`() {
        listOf("ppsspp", "melonds", "azahar").forEach { id ->
            val resource =
                checkNotNull(javaClass.getResourceAsStream("/adapters/$id.json")) {
                    "generated adapter asset is missing: $id"
                }
            val spec =
                resource.bufferedReader(Charsets.UTF_8).use {
                    Json.decodeFromString<AdapterSpec>(it.readText())
                }
            assertEquals(id, spec.id)
        }
    }
}
