package com.retrosave.data.adapters

import android.content.res.AssetManager
import com.retrosave.core.adapters.AdapterSpec
import kotlinx.serialization.json.Json

class AdapterManifestLoader(
    private val assets: AssetManager,
) {
    fun loadActive(): Map<String, AdapterSpec> =
        ACTIVE_ADAPTERS.associateWith { id ->
            val spec =
                assets.open("adapters/$id.json").bufferedReader(Charsets.UTF_8).use {
                    JSON.decodeFromString<AdapterSpec>(it.readText())
                }
            check(spec.id == id) {
                "Adapter asset identity mismatch: expected $id, got ${spec.id}"
            }
            spec
        }

    private companion object {
        val ACTIVE_ADAPTERS = listOf("ppsspp", "melonds", "azahar")
        val JSON =
            Json {
                ignoreUnknownKeys = false
                explicitNulls = true
            }
    }
}
