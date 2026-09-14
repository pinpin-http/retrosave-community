package com.retrosave.data.api

import com.retrosave.core.model.UnitRef
import com.retrosave.core.sync.AuthException
import com.retrosave.core.sync.CasConflictException
import com.retrosave.core.sync.ChecksumMismatchException
import com.retrosave.core.sync.DownloadTarget
import com.retrosave.core.sync.NetworkException
import com.retrosave.core.sync.NotFoundException
import com.retrosave.core.sync.OpenConflictException
import com.retrosave.core.sync.PrepareResult
import com.retrosave.core.sync.RemoteConflict
import com.retrosave.core.sync.RemoteUnit
import com.retrosave.core.sync.RemoteVersion
import com.retrosave.core.sync.ServerException
import com.retrosave.core.sync.SyncApi
import com.retrosave.core.sync.SyncException
import com.retrosave.core.sync.TooLargeException
import com.retrosave.core.sync.UnknownUploadException
import com.retrosave.core.sync.ValidationException
import io.ktor.client.HttpClient
import io.ktor.client.engine.okhttp.OkHttp
import io.ktor.client.plugins.HttpRequestTimeoutException
import io.ktor.client.plugins.HttpTimeout
import io.ktor.client.plugins.contentnegotiation.ContentNegotiation
import io.ktor.client.request.HttpRequestBuilder
import io.ktor.client.request.header
import io.ktor.client.request.prepareGet
import io.ktor.client.request.request
import io.ktor.client.request.setBody
import io.ktor.client.request.url
import io.ktor.client.statement.HttpResponse
import io.ktor.client.statement.bodyAsBytes
import io.ktor.client.statement.bodyAsChannel
import io.ktor.client.statement.bodyAsText
import io.ktor.http.ContentType
import io.ktor.http.HttpHeaders
import io.ktor.http.HttpMethod
import io.ktor.http.content.OutgoingContent
import io.ktor.http.contentType
import io.ktor.http.isSuccess
import io.ktor.serialization.kotlinx.json.json
import io.ktor.utils.io.jvm.javaio.toByteReadChannel
import io.ktor.utils.io.jvm.javaio.toInputStream
import kotlinx.serialization.SerializationException
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.JsonPrimitive
import java.io.File
import java.io.IOException
import java.security.MessageDigest

/** Tampon normatif d'AD-29 : rien de plus gros ne traverse jamais. */
private const val TRANSFER_BUFFER = 64 * 1024

data class ApiCredentials(
    val serverUrl: String,
    val token: String,
    val deviceId: String?,
)

class KtorSyncApi(
    private val credentials: suspend () -> ApiCredentials,
    private val client: HttpClient =
        HttpClient(OkHttp) {
            expectSuccess = false
            install(HttpTimeout) {
                requestTimeoutMillis = 30_000
                connectTimeoutMillis = 10_000
                socketTimeoutMillis = 30_000
            }
            install(ContentNegotiation) {
                json(WireJson)
            }
        },
) : SyncApi {
    override suspend fun health(): Boolean {
        val response =
            request(
                method = HttpMethod.Get,
                path = "/healthz",
                authenticated = false,
                includeDevice = false,
            )
        val body = decode<HealthDto>(response)
        return body.status == "ok" && body.db && body.s3
    }

    override suspend fun registerDevice(
        name: String,
        osName: String,
        appVersion: String,
    ): String {
        val response =
            request(
                method = HttpMethod.Post,
                path = "/v0/devices",
                includeDevice = false,
                jsonBody =
                    WireJson.encodeToString(
                        DeviceCreateDto(
                            name = name,
                            os = osName,
                            appVersion = appVersion,
                        ),
                    ),
            )
        return decode<DeviceCreatedDto>(response).deviceId
    }

    override suspend fun listUnits(): List<RemoteUnit> =
        decode<UnitsResponseDto>(
            request(HttpMethod.Get, "/v0/units"),
        ).units.map(UnitSummaryDto::toRemote)

    override suspend fun createUnit(
        unit: UnitRef,
        idempotencyKey: String,
    ): RemoteUnit {
        val payload =
            UnitCreateDto(
                emulator = unit.emulator,
                unitKey = unit.unitKey,
                unitType = unit.unitType,
                gameKey = unit.gameKey,
                gameLabel = unit.gameLabel,
            )
        return decode<UnitResponseDto>(
            request(
                HttpMethod.Post,
                "/v0/units",
                idempotencyKey = idempotencyKey,
                jsonBody = WireJson.encodeToString(payload),
            ),
        ).unit.toRemote()
    }

    override suspend fun prepareVersion(
        unitId: String,
        baseVersion: Int,
        contentSha256: String,
        archiveSha256: String,
        size: Long,
        archiveBytes: Long,
        idempotencyKey: String,
    ): PrepareResult {
        val payload =
            VersionPrepareDto(
                baseVersion = baseVersion,
                contentSha256 = contentSha256,
                archiveSha256 = archiveSha256,
                size = size,
                archiveBytes = archiveBytes,
            )
        val body =
            decode<PrepareResponseDto>(
                request(
                    HttpMethod.Post,
                    "/v0/units/$unitId/versions:prepare",
                    idempotencyKey = idempotencyKey,
                    jsonBody = WireJson.encodeToString(payload),
                ),
            )
        if (body.duplicate == true && body.version != null) {
            return PrepareResult(duplicateVersion = body.version)
        }
        val upload =
            body.upload
                ?: throw ServerException("Incomplete prepare reply.")
        return PrepareResult(upload = upload.toRemote())
    }

    override suspend fun uploadArchive(
        url: String,
        archive: ByteArray,
    ) {
        transfer(HttpMethod.Put, url, archive)
    }

    override suspend fun upload(
        url: String,
        source: File,
    ) {
        // Ktor lit le fichier au fil de l'envoi : le corps n'existe jamais en
        // une allocation. La longueur est déclarée explicitement, sinon Ktor
        // bascule en chunked et le PUT présigné S3 le refuse.
        val body =
            object : OutgoingContent.ReadChannelContent() {
                override val contentType = ContentType("application", "zstd")
                override val contentLength = source.length()

                override fun readFrom() = source.inputStream().toByteReadChannel()
            }
        val response =
            execute {
                this.method = HttpMethod.Put
                url(url)
                setBody(body)
            }
        checkTransfer(response)
    }

    override suspend fun confirmVersion(
        unitId: String,
        objectKey: String,
        contentSha256: String,
        archiveSha256: String,
        baseVersion: Int,
        environment: Map<String, String>,
        clientMtime: String?,
        idempotencyKey: String,
    ): Int {
        val payload =
            VersionConfirmDto(
                objectKey = objectKey,
                contentSha256 = contentSha256,
                archiveSha256 = archiveSha256,
                baseVersion = baseVersion,
                env = environment.mapValues { JsonPrimitive(it.value) },
                clientMtime = clientMtime,
            )
        return decode<VersionCreatedDto>(
            request(
                HttpMethod.Post,
                "/v0/units/$unitId/versions:confirm",
                idempotencyKey = idempotencyKey,
                jsonBody = WireJson.encodeToString(payload),
            ),
        ).version
    }

    override suspend fun downloadVersion(
        unitId: String,
        number: Int,
    ): DownloadTarget =
        decode<DownloadResponseDto>(
            request(
                HttpMethod.Get,
                "/v0/units/$unitId/versions/$number/download",
            ),
        ).toRemote()

    override suspend fun downloadArchive(url: String): ByteArray = transfer(HttpMethod.Get, url)

    override suspend fun download(
        url: String,
        dest: File,
    ): String {
        dest.parentFile?.mkdirs()
        val digest = MessageDigest.getInstance("SHA-256")
        try {
            client
                .prepareGet(url)
                .execute { response ->
                    checkTransfer(response)
                    response.bodyAsChannel().toInputStream().use { input ->
                        dest.outputStream().buffered(TRANSFER_BUFFER).use { out ->
                            val buffer = ByteArray(TRANSFER_BUFFER)
                            while (true) {
                                val read = input.read(buffer)
                                if (read <= 0) break
                                out.write(buffer, 0, read)
                                digest.update(buffer, 0, read)
                            }
                        }
                    }
                }
        } catch (error: SyncException) {
            dest.delete()
            throw error
        } catch (error: IOException) {
            // Un fichier tronqué serait pris pour une archive valide par
            // l'appelant : ne rien laisser derrière soi.
            dest.delete()
            throw NetworkException(error.message ?: "Network unreachable.", error)
        }
        return digest.digest().joinToString("") { byte ->
            "%02x".format(byte.toInt() and 0xff)
        }
    }

    override suspend fun markMissing(
        unitId: String,
        idempotencyKey: String,
    ) {
        request(
            HttpMethod.Post,
            "/v0/units/$unitId/missing",
            idempotencyKey = idempotencyKey,
            jsonBody = "{}",
        )
    }

    override suspend fun listVersions(unitId: String): Pair<List<RemoteVersion>, Int> {
        val body =
            decode<VersionsResponseDto>(
                request(HttpMethod.Get, "/v0/units/$unitId/versions"),
            )
        return body.versions.map(VersionSummaryDto::toRemote) to body.headVersion
    }

    override suspend fun restore(
        unitId: String,
        number: Int,
        idempotencyKey: String,
    ): Int =
        decode<VersionCreatedDto>(
            request(
                HttpMethod.Post,
                "/v0/units/$unitId/restore",
                idempotencyKey = idempotencyKey,
                jsonBody = WireJson.encodeToString(RestoreRequestDto(number)),
            ),
        ).version

    override suspend fun listConflicts(): List<RemoteConflict> =
        decode<ConflictsResponseDto>(
            request(HttpMethod.Get, "/v0/conflicts?open=1"),
        ).conflicts.map(ConflictSummaryDto::toRemote)

    override suspend fun resolveConflict(
        conflictId: String,
        winner: Int,
        idempotencyKey: String,
    ): Pair<String, Int> {
        val body =
            decode<ResolveResponseDto>(
                request(
                    HttpMethod.Post,
                    "/v0/conflicts/$conflictId/resolve",
                    idempotencyKey = idempotencyKey,
                    jsonBody = WireJson.encodeToString(ResolveRequestDto(winner)),
                ),
            )
        return body.unitId to body.headVersion
    }

    private suspend fun request(
        method: HttpMethod,
        path: String,
        authenticated: Boolean = true,
        includeDevice: Boolean = true,
        idempotencyKey: String? = null,
        jsonBody: String? = null,
    ): HttpResponse {
        val config = credentials()
        val requestUrl = "${config.serverUrl.trimEnd('/')}$path"
        val response =
            execute {
                this.method = method
                url(requestUrl)
                if (authenticated) {
                    header(HttpHeaders.Authorization, "Bearer ${config.token}")
                }
                if (includeDevice) {
                    val deviceId =
                        config.deviceId
                            ?: throw AuthException(
                                "Device not registered: connect RetroSave again.",
                            )
                    header("X-Device-Id", deviceId)
                }
                if (idempotencyKey != null) {
                    header("Idempotency-Key", idempotencyKey)
                }
                if (jsonBody != null) {
                    contentType(ContentType.Application.Json)
                    setBody(jsonBody)
                }
            }
        return checkResponse(response)
    }

    private suspend fun transfer(
        method: HttpMethod,
        url: String,
        content: ByteArray? = null,
    ): ByteArray {
        val response =
            execute {
                this.method = method
                url(url)
                if (content != null) {
                    contentType(ContentType("application", "zstd"))
                    setBody(content)
                }
            }
        checkTransfer(response)
        return if (method == HttpMethod.Get) response.bodyAsBytes() else byteArrayOf()
    }

    /** Même verdict pour les deux chemins de transfert, tamponné ou en flux. */
    private fun checkTransfer(response: HttpResponse) {
        if (response.status.isSuccess()) return
        if (response.status.value >= 500) {
            throw ServerException("Stockage indisponible (${response.status.value}).")
        }
        throw NetworkException("Transfer refused (${response.status.value}).")
    }

    private suspend fun execute(block: HttpRequestBuilder.() -> Unit): HttpResponse =
        try {
            client.request(block)
        } catch (error: SyncException) {
            throw error
        } catch (error: HttpRequestTimeoutException) {
            throw NetworkException(error.message ?: "Network timed out.", error)
        } catch (error: IOException) {
            throw NetworkException(error.message ?: "Network unreachable.", error)
        }

    private suspend fun checkResponse(response: HttpResponse): HttpResponse {
        if (response.status.isSuccess()) {
            return response
        }
        val payload =
            try {
                WireJson.decodeFromString<ErrorEnvelopeDto>(response.bodyAsText())
            } catch (error: SerializationException) {
                throw ServerException(
                    "Invalid server reply (${response.status.value}).",
                )
            }
        val message = payload.error.message
        when (payload.error.code) {
            "invalid_token" -> throw AuthException(message)
            "payload_too_large" -> throw TooLargeException(message)
            "checksum_mismatch" -> throw ChecksumMismatchException(message)
            "unknown_upload" -> throw UnknownUploadException(message)
            "not_found" -> throw NotFoundException(message)
            "cas_conflict" ->
                throw CasConflictException(
                    conflictId = payload.requireConflictId(),
                    head = payload.requireHead().toConflictHead(),
                    yours =
                        payload.yours?.toConflictHead()
                            ?: throw ServerException("Conflit CAS incomplet."),
                )
            "open_conflict" ->
                throw OpenConflictException(
                    conflictId = payload.requireConflictId(),
                    versionA =
                        payload.versionA?.toConflictHead()
                            ?: throw ServerException("Conflit ouvert incomplet."),
                    versionB =
                        payload.versionB?.toConflictHead()
                            ?: throw ServerException("Conflit ouvert incomplet."),
                )
            "validation_error", "not_owner", "already_resolved" ->
                throw ValidationException(message)
            "server_error" -> throw ServerException(message)
            else -> throw ServerException("Erreur serveur inconnue : ${payload.error.code}")
        }
    }

    private suspend inline fun <reified T> decode(response: HttpResponse): T =
        try {
            WireJson.decodeFromString(response.bodyAsText())
        } catch (error: SerializationException) {
            throw ServerException("Invalid JSON reply from the server.")
        }

    private fun ErrorEnvelopeDto.requireConflictId(): String = conflictId ?: throw ServerException("Identifiant de conflit absent.")

    private fun ErrorEnvelopeDto.requireHead(): HeadSummaryDto = head ?: throw ServerException("Missing conflict head.")
}
