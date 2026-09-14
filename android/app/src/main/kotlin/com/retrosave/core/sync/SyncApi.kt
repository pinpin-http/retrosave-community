package com.retrosave.core.sync

import com.retrosave.core.model.UnitRef
import java.io.File

open class SyncException(
    message: String,
    cause: Throwable? = null,
) : RuntimeException(message, cause)

class AuthException(
    message: String,
) : SyncException(message)

class TooLargeException(
    message: String,
) : SyncException(message)

open class ChecksumMismatchException(
    message: String,
) : SyncException(message)

class UnknownUploadException(
    message: String,
) : ChecksumMismatchException(message)

class NotFoundException(
    message: String,
) : SyncException(message)

class NetworkException(
    message: String,
    cause: Throwable? = null,
) : SyncException(message, cause)

class ServerException(
    message: String,
) : SyncException(message)

class ValidationException(
    message: String,
) : SyncException(message)

data class RemoteHead(
    val number: Int,
    val contentSha256: String,
    val sizeBytes: Long,
)

data class RemoteUnit(
    val id: String,
    val emulator: String,
    val unitKey: String,
    val unitType: String,
    val gameKey: String,
    val gameLabel: String,
    // M8 §7. Défaut tolérant : un serveur antérieur ne l'envoie pas, et AD-31
    // veut que le lecteur s'en accommode plutôt que de refuser la réponse.
    val labelSource: String = "auto",
    val headVersion: Int,
    val state: String,
    val head: RemoteHead?,
    // Q45 : l'adresse d'une jaquette, quand le serveur a su la résoudre. Le
    // serveur ne stocke aucune image : il rend une adresse, et c'est le client
    // qui va la chercher puis la garde en cache. Défaut tolérant — un serveur
    // antérieur ne l'envoie pas, et son absence veut dire « pas d'image ».
    val artworkUrl: String? = null,
)

data class UploadTarget(
    val url: String,
    val objectKey: String,
)

data class PrepareResult(
    val duplicateVersion: Int? = null,
    val upload: UploadTarget? = null,
)

data class DownloadTarget(
    val url: String,
    val archiveSha256: String,
    val contentSha256: String,
    val archiveBytes: Long,
)

data class ConflictHead(
    val number: Int,
    val contentSha256: String,
    val sizeBytes: Long,
)

class CasConflictException(
    val conflictId: String,
    val head: ConflictHead,
    val yours: ConflictHead,
) : SyncException("Conflit CAS : $conflictId")

class OpenConflictException(
    val conflictId: String,
    val versionA: ConflictHead,
    val versionB: ConflictHead,
) : SyncException("Conflit ouvert : $conflictId")

data class RemoteVersion(
    val number: Int,
    val kind: String,
    val sizeBytes: Long,
    val contentSha256: String,
    val originDeviceName: String?,
    val clientMtime: String?,
    val createdAt: String,
    val parentNumber: Int?,
)

data class RemoteConflict(
    val id: String,
    val unitId: String,
    val unitLabel: String,
    val versionA: ConflictHead,
    val versionB: ConflictHead,
    val createdAt: String,
)

interface SyncApi {
    suspend fun health(): Boolean

    suspend fun registerDevice(
        name: String,
        osName: String,
        appVersion: String,
    ): String

    suspend fun listUnits(): List<RemoteUnit>

    suspend fun createUnit(
        unit: UnitRef,
        idempotencyKey: String,
    ): RemoteUnit

    suspend fun prepareVersion(
        unitId: String,
        baseVersion: Int,
        contentSha256: String,
        archiveSha256: String,
        size: Long,
        archiveBytes: Long,
        idempotencyKey: String,
    ): PrepareResult

    suspend fun uploadArchive(
        url: String,
        archive: ByteArray,
    )

    /** Envoie une archive stagée sans jamais la tenir en mémoire (AD-29). */
    suspend fun upload(
        url: String,
        source: File,
    )

    suspend fun confirmVersion(
        unitId: String,
        objectKey: String,
        contentSha256: String,
        archiveSha256: String,
        baseVersion: Int,
        environment: Map<String, String>,
        clientMtime: String?,
        idempotencyKey: String,
    ): Int

    suspend fun downloadVersion(
        unitId: String,
        number: Int,
    ): DownloadTarget

    suspend fun downloadArchive(url: String): ByteArray

    /**
     * Copie l'archive sur disque et rend l'empreinte de ce qui est passé.
     *
     * L'empreinte sort de la passe qui écrit : rien n'est relu, et rien de plus
     * gros que le tampon n'existe à la fois (AD-29). L'appelant la compare à
     * l'`archiveSha256` annoncé par le serveur.
     */
    suspend fun download(
        url: String,
        dest: File,
    ): String

    suspend fun markMissing(
        unitId: String,
        idempotencyKey: String,
    )

    suspend fun listVersions(unitId: String): Pair<List<RemoteVersion>, Int>

    suspend fun restore(
        unitId: String,
        number: Int,
        idempotencyKey: String,
    ): Int

    suspend fun listConflicts(): List<RemoteConflict>

    suspend fun resolveConflict(
        conflictId: String,
        winner: Int,
        idempotencyKey: String,
    ): Pair<String, Int>
}
