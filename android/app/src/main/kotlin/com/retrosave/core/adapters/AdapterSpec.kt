package com.retrosave.core.adapters

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class AdapterSpec(
    val id: String,
    val name: String,
    @SerialName("process_names")
    val processNames: List<String>,
    @SerialName("android_packages")
    val androidPackages: List<String>,
    val roots: AdapterRootsSpec,
    val discovery: AdapterDiscoverySpec,
    val identity: AdapterIdentitySpec,
)

@Serializable
data class AdapterRootsSpec(
    val windows: List<String> = emptyList(),
    val linux: List<String> = emptyList(),
    val validate: AdapterRootValidationSpec = AdapterRootValidationSpec(),
)

@Serializable
data class AdapterRootValidationSpec(
    @SerialName("markers_primary")
    val markersPrimary: List<String> = emptyList(),
    @SerialName("markers_secondary")
    val markersSecondary: List<String> = emptyList(),
    @SerialName("markers_secondary_min")
    val markersSecondaryMin: Int = 0,
)

@Serializable
data class AdapterDiscoverySpec(
    @SerialName("unit_type")
    val unitType: String,
    val pattern: String,
    val exclude: List<String>,
    @SerialName("max_depth")
    val maxDepth: Int,
)

@Serializable
data class AdapterIdentitySpec(
    @SerialName("unit_key")
    val unitKey: String,
    @SerialName("game_key")
    val gameKey: String,
)
