package com.retrosave.data.api

import com.retrosave.core.sync.ConflictHead
import com.retrosave.core.sync.DownloadTarget
import com.retrosave.core.sync.RemoteConflict
import com.retrosave.core.sync.RemoteHead
import com.retrosave.core.sync.RemoteUnit
import com.retrosave.core.sync.RemoteVersion
import com.retrosave.core.sync.UploadTarget
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject

internal val WireJson =
    Json {
        encodeDefaults = true
        explicitNulls = true
        // AD-31 : les réponses de l'API sont extensibles. Un lecteur strict
        // transforme tout ajout de champ en rupture pour les clients déjà
        // installés — vérifié le 02/08 : ce drapeau était à `false`, donc
        // ajouter `version` à /healthz aurait cassé la connexion de tout APK
        // en circulation. La validation stricte reste côté serveur, qui valide
        // ce qu'il émet ; les lecteurs, eux, tolèrent.
        ignoreUnknownKeys = true
    }

@Serializable
internal data class HealthDto(
    val status: String,
    val db: Boolean,
    val s3: Boolean,
)

@Serializable
internal data class DeviceCreateDto(
    val name: String,
    val os: String,
    @SerialName("app_version")
    val appVersion: String,
)

@Serializable
internal data class DeviceCreatedDto(
    @SerialName("device_id")
    val deviceId: String,
)

@Serializable
internal data class HeadSummaryDto(
    val number: Int,
    @SerialName("content_sha256")
    val contentSha256: String,
    @SerialName("size_bytes")
    val sizeBytes: Long,
    @SerialName("created_at")
    val createdAt: String,
    @SerialName("origin_device_name")
    val originDeviceName: String?,
) {
    fun toRemoteHead() =
        RemoteHead(
            number = number,
            contentSha256 = contentSha256,
            sizeBytes = sizeBytes,
        )

    fun toConflictHead() =
        ConflictHead(
            number = number,
            contentSha256 = contentSha256,
            sizeBytes = sizeBytes,
        )
}

@Serializable
internal data class UnitSummaryDto(
    val id: String,
    val emulator: String,
    @SerialName("unit_key")
    val unitKey: String,
    @SerialName("unit_type")
    val unitType: String,
    @SerialName("game_key")
    val gameKey: String,
    @SerialName("game_label")
    val gameLabel: String,
    // Défaut tolérant (AD-31) : un serveur antérieur à M8 ne l'envoie pas.
    @SerialName("label_source")
    val labelSource: String = "auto",
    @SerialName("head_version")
    val headVersion: Int,
    val state: String,
    @SerialName("updated_at")
    val updatedAt: String,
    val head: HeadSummaryDto?,
    // Q45, additif : absent d'un serveur antérieur.
    @SerialName("artwork_url")
    val artworkUrl: String? = null,
) {
    fun toRemote() =
        RemoteUnit(
            id = id,
            emulator = emulator,
            unitKey = unitKey,
            unitType = unitType,
            gameKey = gameKey,
            gameLabel = gameLabel,
            labelSource = labelSource,
            headVersion = headVersion,
            state = state,
            head = head?.toRemoteHead(),
            // Une adresse qui n'est pas en HTTPS est ignorée : une jaquette ne
            // vaut pas qu'on aille chercher quoi que ce soit en clair.
            artworkUrl = artworkUrl?.takeIf { it.startsWith("https://") },
        )
}

@Serializable
internal data class UnitsResponseDto(
    val units: List<UnitSummaryDto>,
)

@Serializable
internal data class UnitResponseDto(
    val unit: UnitSummaryDto,
)

@Serializable
internal data class UnitCreateDto(
    val emulator: String,
    @SerialName("unit_key")
    val unitKey: String,
    @SerialName("unit_type")
    val unitType: String,
    @SerialName("game_key")
    val gameKey: String,
    @SerialName("game_label")
    val gameLabel: String,
)

@Serializable
internal data class VersionPrepareDto(
    @SerialName("base_version")
    val baseVersion: Int,
    @SerialName("content_sha256")
    val contentSha256: String,
    @SerialName("archive_sha256")
    val archiveSha256: String,
    val size: Long,
    @SerialName("archive_bytes")
    val archiveBytes: Long,
)

@Serializable
internal data class UploadTargetDto(
    val url: String,
    @SerialName("object_key")
    val objectKey: String,
    @SerialName("expires_at")
    val expiresAt: String,
) {
    fun toRemote() =
        UploadTarget(
            url = url,
            objectKey = objectKey,
        )
}

@Serializable
internal data class PrepareResponseDto(
    val duplicate: Boolean? = null,
    val version: Int? = null,
    val upload: UploadTargetDto? = null,
)

@Serializable
internal data class VersionConfirmDto(
    @SerialName("object_key")
    val objectKey: String,
    @SerialName("content_sha256")
    val contentSha256: String,
    @SerialName("archive_sha256")
    val archiveSha256: String,
    @SerialName("base_version")
    val baseVersion: Int,
    val env: Map<String, JsonElement>,
    @SerialName("client_mtime")
    val clientMtime: String?,
)

@Serializable
internal data class VersionCreatedDto(
    val version: Int,
)

@Serializable
internal data class VersionSummaryDto(
    val number: Int,
    val kind: String,
    @SerialName("size_bytes")
    val sizeBytes: Long,
    @SerialName("content_sha256")
    val contentSha256: String,
    @SerialName("origin_device_name")
    val originDeviceName: String?,
    @SerialName("client_mtime")
    val clientMtime: String?,
    @SerialName("created_at")
    val createdAt: String,
    @SerialName("parent_number")
    val parentNumber: Int?,
) {
    fun toRemote() =
        RemoteVersion(
            number = number,
            kind = kind,
            sizeBytes = sizeBytes,
            contentSha256 = contentSha256,
            originDeviceName = originDeviceName,
            clientMtime = clientMtime,
            createdAt = createdAt,
            parentNumber = parentNumber,
        )
}

@Serializable
internal data class VersionsResponseDto(
    val versions: List<VersionSummaryDto>,
    @SerialName("head_version")
    val headVersion: Int,
)

@Serializable
internal data class DownloadResponseDto(
    val url: String,
    @SerialName("archive_sha256")
    val archiveSha256: String,
    @SerialName("content_sha256")
    val contentSha256: String,
    @SerialName("archive_bytes")
    val archiveBytes: Long,
    @SerialName("expires_at")
    val expiresAt: String,
) {
    fun toRemote() =
        DownloadTarget(
            url = url,
            archiveSha256 = archiveSha256,
            contentSha256 = contentSha256,
            archiveBytes = archiveBytes,
        )
}

@Serializable
internal data class RestoreRequestDto(
    val version: Int,
)

@Serializable
internal data class ConflictSummaryDto(
    val id: String,
    @SerialName("unit_id")
    val unitId: String,
    @SerialName("unit_label")
    val unitLabel: String,
    @SerialName("version_a")
    val versionA: HeadSummaryDto,
    @SerialName("version_b")
    val versionB: HeadSummaryDto,
    @SerialName("created_at")
    val createdAt: String,
) {
    fun toRemote() =
        RemoteConflict(
            id = id,
            unitId = unitId,
            unitLabel = unitLabel,
            versionA = versionA.toConflictHead(),
            versionB = versionB.toConflictHead(),
            createdAt = createdAt,
        )
}

@Serializable
internal data class ConflictsResponseDto(
    val conflicts: List<ConflictSummaryDto>,
)

@Serializable
internal data class ResolveRequestDto(
    val winner: Int,
)

@Serializable
internal data class ResolveResponseDto(
    @SerialName("unit_id")
    val unitId: String,
    @SerialName("head_version")
    val headVersion: Int,
)

@Serializable
internal data class ApiErrorDto(
    val code: String,
    val message: String,
    val details: JsonObject,
)

@Serializable
internal data class ErrorEnvelopeDto(
    val error: ApiErrorDto,
    @SerialName("conflict_id")
    val conflictId: String? = null,
    val head: HeadSummaryDto? = null,
    val yours: HeadSummaryDto? = null,
    @SerialName("version_a")
    val versionA: HeadSummaryDto? = null,
    @SerialName("version_b")
    val versionB: HeadSummaryDto? = null,
)
