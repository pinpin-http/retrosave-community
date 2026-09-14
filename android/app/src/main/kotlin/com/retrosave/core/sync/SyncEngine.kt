package com.retrosave.core.sync

import com.retrosave.core.archive.ArchiveSource
import com.retrosave.core.archive.ContentDigest
import com.retrosave.core.archive.ContentFile
import com.retrosave.core.archive.contentSha256
import com.retrosave.core.archive.createArchiveFrom
import com.retrosave.core.archive.directoryContentSha256
import com.retrosave.core.archive.extractArchiveTo
import com.retrosave.core.fingerprint.quickFingerprintHash
import com.retrosave.core.fs.RootInaccessibleException
import com.retrosave.core.fs.UnsupportedNodeException
import com.retrosave.core.fs.Vfs
import com.retrosave.core.model.DiscoveredUnit
import com.retrosave.core.model.LocalUnit
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import com.retrosave.core.model.SyncReport
import com.retrosave.core.model.UnitRef
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.time.Instant
import java.util.UUID

private const val DEFAULT_STABILIZATION_MS = 10_000L
private const val MISSING_PAUSE_THRESHOLD = 10

fun interface Clock {
    fun nowMs(): Long
}

fun interface Sleeper {
    suspend fun sleep(delayMs: Long)
}

fun interface UnitDiscovery {
    suspend fun discover(): List<DiscoveredUnit>
}

fun interface RemoteTargetResolver {
    fun resolve(remote: RemoteUnit): UnitRef?
}

data class ScanRow(
    val unitKey: String,
    val state: String,
    val changed: Boolean,
)

data class ScanReport(
    val rows: List<ScanRow>,
    val missing: List<LocalUnit>,
    val paused: Boolean,
)

class SyncEngine(
    private val vfs: Vfs,
    private val api: SyncApi,
    private val store: UnitStore,
    private val discovery: UnitDiscovery,
    private val configuredRoots: suspend () -> Map<String, List<RootRef>>,
    private val remoteTarget: RemoteTargetResolver? = null,
    private val isEmulatorRunning: (String) -> Boolean = { false },
    private val clock: Clock = Clock(System::currentTimeMillis),
    private val sleeper: Sleeper = Sleeper { delay(it) },
    private val environment: Map<String, String> = emptyMap(),
    private val stabilizationMs: Long = DEFAULT_STABILIZATION_MS,
) {
    suspend fun scan(): ScanReport {
        val now = clock.nowMs()
        val rows = mutableListOf<ScanRow>()
        val discoveredByRoot = mutableMapOf<Pair<String, String>, MutableSet<String>>()
        discovery.discover().forEach { candidate ->
            val unit = candidate.unit
            discoveredByRoot
                .getOrPut(unit.emulator to unit.root.rootId, ::mutableSetOf)
                .add(unit.unitKey)
            val qfHash = quickFingerprintHash(candidate.nodes)
            var (local, changed) =
                store.upsertScan(
                    unit = unit,
                    qfHash = qfHash,
                    nodes = candidate.nodes,
                    now = now,
                )
            if (candidate.unsupportedReason != null) {
                store.markUnsupported(local.localId)
                local = refresh(local)
                store.journal(
                    now = now,
                    event = "unsupported",
                    detail = candidate.unsupportedReason,
                    localId = local.localId,
                )
            } else if (candidate.duplicateRelPaths.isNotEmpty()) {
                store.markDuplicate(local.localId)
                local = refresh(local)
                store.journal(
                    now = now,
                    event = "duplicate_quarantine",
                    detail = candidate.duplicateRelPaths.joinToString(prefix = "[", postfix = "]"),
                    localId = local.localId,
                )
            }
            rows += ScanRow(local.unitKey, local.state, changed)
        }

        val missing = mutableListOf<LocalUnit>()
        configuredRoots().forEach { (emulator, emulatorRoots) ->
            emulatorRoots.forEach { root ->
                missing +=
                    store.markMissingExcept(
                        emulator = emulator,
                        rootId = root.rootId,
                        discoveredKeys =
                            discoveredByRoot[emulator to root.rootId].orEmpty(),
                    )
            }
        }
        val paused = shouldPauseForMissing(missing.size, MISSING_PAUSE_THRESHOLD)
        if (paused) {
            store.pauseAll()
            store.journal(
                now = now,
                event = "paused_missing_threshold",
                detail = """{"missing":${missing.size}}""",
            )
        }
        return ScanReport(rows, missing, paused)
    }

    suspend fun sync(): SyncReport {
        purgeStagingResidue()
        val report = SyncReport()
        resumeApplyJournals(report)
        val scan = scan()
        if (scan.paused) {
            report.paused = true
            report.messages +=
                "Beaucoup de sauvegardes ont disparu localement — synchronisation en pause."
            return report
        }
        sendMissing(scan.missing, report)
        val remoteUnits: List<RemoteUnit>
        val conflicts: List<RemoteConflict>
        try {
            remoteUnits = retryNetwork(api::listUnits)
            conflicts = retryNetwork(api::listConflicts)
        } catch (error: SyncException) {
            report.retryable = error is NetworkException
            recordError(report, "Liste distante indisponible : ${error.message}")
            return report
        }

        bindRemoteUnits(remoteUnits, report)
        val conflictByUnit = conflicts.associateBy(RemoteConflict::unitId)
        refreshConflictState(conflictByUnit)

        remoteUnits.forEach { remote ->
            val local = store.getUnit(remote.emulator, remote.unitKey)
            if (local != null && local.state !in NON_ACTIVE_STATES) {
                // AD-28 : une unité empoisonnée ne fait jamais tomber la passe.
                // AD-30 : et si elle la fait tomber quand même, elle ne
                // recommence pas indéfiniment.
                if (!quarantined(local, remote.headVersion, report)) {
                    withUnitIsolation(local, remote.headVersion, report) {
                        pullOne(local, remote, conflictByUnit[remote.id], report)
                    }
                }
            }
        }

        val remoteByIdentity = remoteUnits.associateBy { it.emulator to it.unitKey }
        reconcileLabels(remoteByIdentity)
        store.listUnits().forEach { local ->
            if (local.state != "active" || local.openConflictId != null) {
                return@forEach
            }
            val refreshed = refresh(local)
            if (refreshed.applyJournalContentSha256 != null) {
                // AD-27 : une application encore ouverte ne se pousse jamais.
                // Le contenu local peut être déchiré ; la reprise le tranchera.
                return@forEach
            }
            val remote = remoteByIdentity[local.emulator to local.unitKey]
            if (remote != null && refreshed.lastSyncedVersion != remote.headVersion) {
                return@forEach
            }
            val head = remoteByIdentity[local.emulator to local.unitKey]?.headVersion
            if (!quarantined(refreshed, head, report)) {
                withUnitIsolation(refreshed, head, report) {
                    pushOne(refreshed, report)
                }
            }
        }
        return report
    }

    /** Lecteur unique des colonnes du marqueur (Q19). */
    private fun journalOf(local: LocalUnit): ApplyJournal =
        readApplyJournal(
            local.applyJournalVersion,
            local.applyJournalContentSha256,
            local.applyJournalStartedAt,
        )

    /**
     * Solde les applications interrompues avant le scan — comptabilité seule.
     *
     * Depuis Q19 la reprise ne télécharge jamais : elle clôt ce qui avait déjà
     * abouti et laisse le reste sous marqueur, pour que le PULL applique la
     * tête courante et non celle que visait le marqueur.
     */
    private suspend fun resumeApplyJournals(report: SyncReport) {
        store.listUnits().forEach { local ->
            when (val journal = journalOf(local)) {
                is ApplyJournal.Absent -> return@forEach
                is ApplyJournal.Malformed ->
                    // Impossible depuis Q17 : les trois colonnes bougent
                    // ensemble. S'il en surgit un, on veut la trace ; l'unité
                    // reste sous marqueur et le PULL appliquera la tête.
                    store.journal(
                        now = clock.nowMs(),
                        event = "apply_journal_malformed",
                        detail = """{"unit_key":"${local.unitKey}"}""",
                        localId = local.localId,
                    )
                is ApplyJournal.Present ->
                    try {
                        settleApplyJournal(local, journal)
                    } catch (error: CancellationException) {
                        throw error
                    } catch (error: Exception) {
                        // AD-28 : une reprise qui échoue ne fait pas tomber la passe.
                        if (error.isExpectedSyncFailure()) {
                            recordError(
                                report,
                                "Reprise de ${local.unitKey} impossible : ${error.message}",
                                local.localId,
                            )
                        } else {
                            throw error
                        }
                    }
            }
        }
    }

    private suspend fun settleApplyJournal(
        local: LocalUnit,
        journal: ApplyJournal.Present,
    ) {
        val unit = unitRef(local)
        if (applyJournalDecision(journal, localContent(unit)) != APPLY_CLOSE) {
            // reapply : rien ici. L'unité reste sous marqueur, donc exclue du
            // push, et le PULL de cette passe appliquera la tête courante.
            return
        }
        // L'écriture avait abouti, seule la comptabilité manquait : aucun
        // téléchargement, et pas une écriture de plus sur la cible.
        val version = local.applyJournalVersion ?: return
        store.setSynced(
            localId = local.localId,
            version = version,
            contentSha256 = journal.expectedContentSha256,
        )
        store.journal(
            now = clock.nowMs(),
            event = "apply_journal_closed",
            detail = """{"version":$version}""",
            localId = local.localId,
        )
    }

    suspend fun pullRemoteUnit(
        remote: RemoteUnit,
        report: SyncReport,
    ) {
        bindRemoteUnits(listOf(remote), null)
        val local = store.getUnit(remote.emulator, remote.unitKey)
        if (local == null) {
            recordError(report, "Local save not found for PULL.")
            return
        }
        pullOne(local, remote, null, report)
    }

    fun emulatorRunning(emulator: String): Boolean = isEmulatorRunning(emulator)

    private suspend fun bindRemoteUnits(
        remoteUnits: List<RemoteUnit>,
        report: SyncReport?,
    ) {
        val rootsByEmulator = configuredRoots()
        remoteUnits.forEach { remote ->
            val local = store.getUnit(remote.emulator, remote.unitKey)
            if (local != null) {
                if (local.serverId != remote.id) {
                    store.bindServer(local.localId, remote.id)
                }
                return@forEach
            }
            val target = remoteTarget?.resolve(remote)
            if (target == null) {
                // §9.6 : deux causes, deux remèdes. Sans racine configurée,
                // l'utilisateur doit choisir un dossier — pas lancer un jeu.
                // Ne pas distinguer, c'est l'envoyer faire la mauvaise chose.
                val configured = rootsByEmulator[remote.emulator].orEmpty().isNotEmpty()
                if (report != null) {
                    if (!configured) {
                        report.unconfiguredAdapters += remote.emulator
                    } else if (remote.unitKey !in report.awaitingPlacement) {
                        report.awaitingPlacement += remote.unitKey
                    }
                }
                store.journal(
                    now = clock.nowMs(),
                    event = if (configured) "awaiting_placement" else "adapter_unconfigured",
                    detail =
                        """{"emulator":"${remote.emulator}","unit_key":"${remote.unitKey}"}""",
                )
                return@forEach
            }
            store.addRemotePlaceholder(target, remote.id)
        }
    }

    private suspend fun refreshConflictState(conflictByUnit: Map<String, RemoteConflict>) {
        store.listUnits().forEach { local ->
            val serverId = local.serverId ?: return@forEach
            val conflict = conflictByUnit[serverId]
            if (conflict != null) {
                // Re-lier un conflit déjà connu ne doit pas effacer la branche
                // qu'on a nous-mêmes poussée : c'est elle qui, à la résolution,
                // dit au PULL « ce contenu local est déjà sur le serveur, ne le
                // repousse pas ». Sans ça, garder l'autre branche rouvrait un
                // conflit à chaque tentative.
                store.setConflict(
                    localId = local.localId,
                    conflictId = conflict.id,
                    pendingBranchNumber = local.pendingBranchNumber,
                    pendingBranchContentSha256 = local.pendingBranchContentSha256,
                )
            } else if (local.openConflictId != null) {
                store.clearOpenConflict(local.localId)
            }
        }
    }

    /**
     * AD-30 : compter avant de traiter, pour survivre à ce qui tue le process.
     *
     * L'incrément est écrit *avant* le traitement et validé tout de suite. C'est
     * ce qui le sépare d'un `catch` : un OOM, un crash natif de zstd-jni ou un
     * kill système n'exécutent aucun bloc de reprise, mais aucun d'eux ne peut
     * défaire une écriture déjà commise.
     */
    private suspend fun withUnitIsolation(
        local: LocalUnit,
        headVersion: Int?,
        report: SyncReport,
        block: suspend () -> Unit,
    ) {
        store.recordFailure(local.localId, headVersion)
        try {
            block()
            store.clearFailures(local.localId)
        } catch (error: CancellationException) {
            store.clearFailures(local.localId)
            throw error
        } catch (error: Throwable) {
            // Q21 : `Throwable`, pas `Exception`. Un `OutOfMemoryError` étend
            // `Error` et traversait cette frontière sans être vu, emportant le
            // process avec lui.
            if (error is SyncException) {
                store.clearFailures(local.localId)
                throw error
            }
            store.markError(local.localId)
            recordError(
                report,
                "${local.unitKey} : ${error::class.simpleName} — ${error.message}",
                local.localId,
            )
        }
    }

    /**
     * AD-30 : sauter une unité qui a fait tomber le process trois fois.
     *
     * La quarantaine ne vaut que pour la tête serveur qui l'a provoquée : une
     * nouvelle version réarme d'elle-même, le poison étant le plus souvent lié
     * à un contenu précis.
     */
    private suspend fun quarantined(
        local: LocalUnit,
        headVersion: Int?,
        report: SyncReport,
    ): Boolean {
        val current = refresh(local)
        if (current.consecutiveFailures < QUARANTINE_THRESHOLD) return false
        if (current.lastFailureHead != headVersion) {
            store.clearFailures(current.localId)
            return false
        }
        store.markError(current.localId)
        recordError(
            report,
            "${current.unitKey}: repeated failures, quarantined — " +
                "tap Retry once the cause is fixed.",
            current.localId,
        )
        return true
    }

    private suspend fun pullOne(
        local: LocalUnit,
        remote: RemoteUnit,
        conflict: RemoteConflict?,
        report: SyncReport,
    ) {
        if (!mayApplyWithEmulator(isEmulatorRunning(local.emulator))) return
        // Ordre de priorité §6.5. Le marqueur prime sur le blocage « conflit
        // ouvert » : laisser du contenu déchiré sur disque pendant que
        // l'utilisateur arbitre serait pire, et la tête est de toute façon l'un
        // des deux candidats.
        val underMarker = journalOf(local) !is ApplyJournal.Absent
        if (!underMarker && (conflict != null || local.openConflictId != null)) return
        val head = remote.head ?: return
        if (remote.headVersion == 0) return
        if (
            !underMarker &&
            local.lastSyncedVersion == remote.headVersion &&
            local.lastSyncedContentSha256 == head.contentSha256
        ) {
            return
        }

        val unit = unitRef(local)
        val localContent =
            try {
                localContent(unit)
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                if (error.isExpectedSyncFailure()) {
                    report.retryable = report.retryable || error is NetworkException
                    recordError(
                        report,
                        "Lecture locale ${local.unitKey} impossible : ${error.message}",
                        local.localId,
                    )
                    return
                }
                throw error
            }
        val pendingMatches =
            localContent != null &&
                local.pendingBranchContentSha256 == localContent
        if (localContent == head.contentSha256) {
            store.setSynced(
                localId = local.localId,
                version = remote.headVersion,
                contentSha256 = head.contentSha256,
                clearPending = true,
            )
            report.pulled += 1
            return
        }
        if (
            !underMarker &&
            localContent != null &&
            !pendingMatches &&
            localContent != local.lastSyncedContentSha256
        ) {
            // Sous marqueur ce test est sauté : le contenu déchiré est le
            // nôtre, pas une modification du joueur, et ne doit jamais partir.
            if (!stable(local)) {
                store.journal(
                    now = clock.nowMs(),
                    event = "pull_waits_for_stable_push",
                    detail = "{}",
                    localId = local.localId,
                )
                return
            }
            val conflictsBefore = report.conflicts
            pushOne(local, report)
            val refreshed = refresh(local)
            if (
                report.conflicts > conflictsBefore ||
                refreshed.openConflictId != null
            ) {
                return
            }
            if (refreshed.lastSyncedVersion == remote.headVersion) return
        }

        try {
            val target =
                retryNetwork {
                    api.downloadVersion(remote.id, remote.headVersion)
                }
            val scratch = newScratch("pull")
            try {
                val archiveFile = File(scratch, "archive.tar.zst")
                val actualArchiveSha256 = retryNetwork { api.download(target.url, archiveFile) }
                require(archiveFile.length() == target.archiveBytes) {
                    "archive download size does not match"
                }
                if (actualArchiveSha256 != target.archiveSha256) {
                    store.journal(
                        now = clock.nowMs(),
                        event = "archive_sha256_warning",
                        detail =
                            """{"expected":"${target.archiveSha256}","actual":"$actualArchiveSha256"}""",
                        localId = local.localId,
                    )
                }
                // Ordre normatif §6.5 (Q24) : étalement, verdict, backup, marqueur,
                // application, setSynced. Le marqueur ne doit jamais exister sans
                // que le backup soit pris — sinon l'état d'avant le pull reste sans
                // copie et la reprise ne la refera jamais.
                if (!mayApplyWithEmulator(isEmulatorRunning(local.emulator))) return
                applyPull(
                    unit = unit,
                    archive = archiveFile,
                    staging = File(scratch, "staging"),
                    expectedContentSha256 = target.contentSha256,
                    skipBackup = underMarker,
                ) {
                    store.setApplyJournal(
                        localId = local.localId,
                        version = remote.headVersion,
                        contentSha256 = target.contentSha256,
                        now = clock.nowMs(),
                    )
                }
                val nodes = scanUnitNodes(unit)
                val qfHash = quickFingerprintHash(nodes)
                val now = clock.nowMs()
                store.upsertScan(unit, qfHash, nodes, now)
                store.setSynced(
                    localId = local.localId,
                    version = remote.headVersion,
                    contentSha256 = target.contentSha256,
                    qfHash = qfHash,
                    now = now,
                    clearPending = true,
                )
                store.journal(
                    now = now,
                    event = "pulled",
                    detail = """{"version":${remote.headVersion}}""",
                    localId = local.localId,
                )
                report.pulled += 1
            } finally {
                scratch.deleteRecursively()
            }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            if (error is RootInaccessibleException) {
                // Q20 : signal typé, jamais le texte de l'exception à l'écran.
                // Celle-ci porte un identifiant interne, utile au journal et à
                // personne d'autre.
                report.inaccessibleAdapters += local.emulator
                report.errors += 1
            } else if (error.isExpectedSyncFailure()) {
                recordError(
                    report,
                    "PULL ${local.unitKey} impossible : ${error.message}",
                    local.localId,
                )
            } else {
                throw error
            }
        }
    }

    private suspend fun pushOne(
        initial: LocalUnit,
        report: SyncReport,
    ) {
        if (!stable(initial)) return
        var local = initial
        val unit = unitRef(local)
        try {
            val nodesBefore = scanUnitNodes(unit)
            if (nodesBefore.isEmpty()) return
            val qfBefore = quickFingerprintHash(nodesBefore)
            if (qfBefore != local.observedQfHash) {
                store.upsertScan(unit, qfBefore, nodesBefore, clock.nowMs())
                return
            }
            val scratch = newScratch("push")
            val archiveFile = File(scratch, "archive.tar.zst")
            val blob = createArchiveFrom(unit.unitType, captureSources(unit, nodesBefore), archiveFile)
            val nodesAfter = scanUnitNodes(unit)
            val qfAfter = quickFingerprintHash(nodesAfter)
            if (qfAfter != qfBefore) {
                store.upsertScan(unit, qfAfter, nodesAfter, clock.nowMs())
                store.journal(
                    now = clock.nowMs(),
                    event = "capture_changed",
                    detail = "{}",
                    localId = local.localId,
                )
                return
            }
            if (blob.contentSha256 == local.lastSyncedContentSha256) return
            if (local.serverId == null) {
                val remote =
                    retryNetwork {
                        api.createUnit(unit, UUID.randomUUID().toString())
                    }
                store.bindServer(local.localId, remote.id)
                local = refresh(local)
            }
            val serverId = checkNotNull(local.serverId)
            val confirmKey = UUID.randomUUID().toString()
            var networkFailureSeen = false
            repeat(3) {
                try {
                    val prepared =
                        retryNetwork {
                            api.prepareVersion(
                                unitId = serverId,
                                baseVersion = local.lastSyncedVersion,
                                contentSha256 = blob.contentSha256,
                                archiveSha256 = blob.archiveSha256,
                                size = blob.sizeBytes,
                                archiveBytes = blob.archiveBytes,
                                idempotencyKey = UUID.randomUUID().toString(),
                            )
                        }
                    if (prepared.duplicateVersion != null) {
                        store.setSynced(
                            localId = local.localId,
                            version = prepared.duplicateVersion,
                            contentSha256 = blob.contentSha256,
                        )
                        return
                    }
                    val upload =
                        prepared.upload
                            ?: error("prepare response has no upload target")
                    api.upload(upload.url, archiveFile)
                    val version =
                        retryNetwork {
                            api.confirmVersion(
                                unitId = serverId,
                                objectKey = upload.objectKey,
                                contentSha256 = blob.contentSha256,
                                archiveSha256 = blob.archiveSha256,
                                baseVersion = local.lastSyncedVersion,
                                environment = environment,
                                clientMtime = clientMtime(nodesAfter),
                                idempotencyKey = confirmKey,
                            )
                        }
                    store.setSynced(
                        localId = local.localId,
                        version = version,
                        contentSha256 = blob.contentSha256,
                    )
                    store.journal(
                        now = clock.nowMs(),
                        event = "pushed",
                        detail = """{"version":$version}""",
                        localId = local.localId,
                    )
                    report.pushed += 1
                    return
                } catch (_: ChecksumMismatchException) {
                    // The object reservation is discarded by preparing again.
                } catch (_: NetworkException) {
                    networkFailureSeen = true
                    // A new prepare safely recovers an interrupted direct transfer.
                }
            }
            report.retryable = report.retryable || networkFailureSeen
            recordError(
                report,
                "PUSH ${local.unitKey} gave up after two re-prepares.",
                local.localId,
            )
        } catch (error: CancellationException) {
            throw error
        } catch (conflict: CasConflictException) {
            store.setConflict(
                localId = local.localId,
                conflictId = conflict.conflictId,
                pendingBranchNumber = conflict.yours.number,
                pendingBranchContentSha256 = conflict.yours.contentSha256,
            )
            report.conflicts += 1
        } catch (conflict: OpenConflictException) {
            store.setConflict(local.localId, conflict.conflictId)
            report.conflicts += 1
        } catch (_: TooLargeException) {
            store.markUnsupported(local.localId)
            recordError(
                report,
                "${local.unitKey} is over 256 MB.",
                local.localId,
            )
        } catch (error: Exception) {
            if (error.isExpectedSyncFailure()) {
                report.retryable = report.retryable || error is NetworkException
                recordError(
                    report,
                    "PUSH ${local.unitKey} impossible : ${error.message}",
                    local.localId,
                )
            } else {
                throw error
            }
        }
    }

    private suspend fun sendMissing(
        missing: List<LocalUnit>,
        report: SyncReport,
    ) {
        missing.forEach { unit ->
            val serverId = unit.serverId ?: return@forEach
            try {
                api.markMissing(serverId, UUID.randomUUID().toString())
            } catch (error: SyncException) {
                report.retryable = report.retryable || error is NetworkException
                recordError(
                    report,
                    "Signalement missing impossible : ${error.message}",
                    unit.localId,
                )
            }
        }
    }

    /**
     * AD-29 : la contrainte par appareil est le disque, pas la mémoire.
     *
     * Streamer supprime le plafond mémoire mais pas le besoin de place : il
     * faut l'archive **et** son contenu étalé avant de pouvoir écrire, plus une
     * marge. Échouer ici donne un message actionnable, au lieu d'un « no space
     * left on device » au milieu d'une application.
     */
    private fun requireFreeSpace(
        scratch: File,
        archiveBytes: Long,
        sizeBytes: Long,
        local: LocalUnit,
    ) {
        val needed = ((archiveBytes + sizeBytes) * 12) / 10
        val free = scratch.usableSpace
        if (free >= needed) return
        val missingMb = (needed - free) / (1024 * 1024) + 1
        throw IOException(
            "Not enough space for ${local.unitKey}: free $missingMb MB.",
        )
    }

    private fun newScratch(purpose: String): File {
        // Q23 : zone de transit hors de toute racine surveillée — rien ne doit
        // affleurer en SAF — et emplacement fixe pour que les résidus d'un
        // crash soient purgeables.
        val root = File(System.getProperty("java.io.tmpdir"), "retrosave-staging")
        root.mkdirs()
        val directory = File(root, "rsc-$purpose-${System.nanoTime()}")
        check(directory.mkdirs()) { "cannot create scratch directory" }
        return directory
    }

    private fun purgeStagingResidue() {
        val root = File(System.getProperty("java.io.tmpdir"), "retrosave-staging")
        root.listFiles()?.forEach { it.deleteRecursively() }
    }

    private suspend fun applyPull(
        unit: UnitRef,
        archive: File,
        staging: File,
        expectedContentSha256: String,
        skipBackup: Boolean,
        beforeFirstWrite: suspend () -> Unit,
    ) {
        val prefix = unit.relPath.trimEnd('/').let { if (it.isEmpty()) "" else "$it/" }
        var started = false
        var existing: List<NodeMeta> = emptyList()

        val digests =
            extractArchiveTo(archive, unit.unitType, expectedContentSha256, staging) {
                relPath,
                staged,
                sha256,
                ->
                if (!started) {
                    // Le verdict de contenu est rendu : à partir d'ici on touche
                    // les données de l'utilisateur. Backup puis marqueur, une
                    // seule fois, dans cet ordre (Q24).
                    started = true
                    if (vfs.exists(unit.root, unit.relPath)) {
                        existing = scanUnitNodes(unit)
                        // AD-27 : sous marqueur le backup en place vaut mieux que
                        // le disque ; le refaire le perdrait par rotation.
                        if (!skipBackup) {
                            vfs.backup(unit.root, unit.relPath)
                        }
                    }
                    beforeFirstWrite()
                }
                val targetPath = if (unit.unitType == "file") unit.relPath else "$prefix$relPath"
                staged.inputStream().buffered().use { stream ->
                    vfs.writeAtomic(unit.root, targetPath, stream, sha256)
                }
            }

        if (unit.unitType == "file") return
        val written = digests.map { it.relPath }.toSet()
        existing
            .filter { it.relPath !in written }
            .forEach { vfs.delete(unit.root, "$prefix${it.relPath}") }
    }

    private fun captureSources(
        unit: UnitRef,
        nodes: List<NodeMeta>,
    ): List<ArchiveSource> {
        // Q26 : un nom, une taille, et de quoi ouvrir — jamais les octets. Un
        // document SAF n'a pas de chemin de système de fichiers.
        if (unit.unitType == "file") {
            return listOf(
                ArchiveSource(
                    relPath = unit.relPath.substringAfterLast('/'),
                    sizeBytes = nodes.firstOrNull()?.sizeBytes ?: 0L,
                ) { vfs.openRead(unit.root, unit.relPath) },
            )
        }
        val prefix = unit.relPath.trimEnd('/').let { if (it.isEmpty()) "" else "$it/" }
        return nodes.map { node ->
            ArchiveSource(relPath = node.relPath, sizeBytes = node.sizeBytes) {
                vfs.openRead(unit.root, "$prefix${node.relPath}")
            }
        }
    }

    private suspend fun scanUnitNodes(unit: UnitRef): List<NodeMeta> {
        if (unit.unitType == "file") {
            val name = unit.relPath.substringAfterLast('/')
            val node = vfs.listFiles(unit.root, unit.relPath).firstOrNull()
            return if (node == null) {
                emptyList()
            } else {
                listOf(NodeMeta(name, node.sizeBytes, node.mtimeMs))
            }
        }
        val prefix = unit.relPath.trimEnd('/').let { if (it.isEmpty()) "" else "$it/" }
        return vfs.listFiles(unit.root, unit.relPath).map { node ->
            NodeMeta(
                relPath = node.relPath.removePrefix(prefix),
                sizeBytes = node.sizeBytes,
                mtimeMs = node.mtimeMs,
            )
        }
    }

    private suspend fun localContent(unit: UnitRef): String? {
        return try {
            if (!vfs.exists(unit.root, unit.relPath)) return null
            val nodes = scanUnitNodes(unit)
            if (nodes.isEmpty()) return null
            // AD-29 : l'identité se calcule à partir des empreintes par
            // fichier, jamais en tenant l'unité. `vfs.sha256` streame déjà.
            if (unit.unitType == "file") {
                vfs.sha256(unit.root, unit.relPath).joinToString("") {
                    "%02x".format(it.toInt() and 0xff)
                }
            } else {
                val prefix = unit.relPath.trimEnd('/').let { if (it.isEmpty()) "" else "$it/" }
                directoryContentSha256(
                    nodes.map { node ->
                        ContentDigest(
                            relPath = node.relPath,
                            sizeBytes = node.sizeBytes,
                            sha256Hex =
                                vfs.sha256(unit.root, "$prefix${node.relPath}").joinToString("") {
                                    "%02x".format(it.toInt() and 0xff)
                                },
                        )
                    },
                )
            }
        } catch (_: IOException) {
            null
        }
    }

    private fun stable(local: LocalUnit): Boolean =
        isStable(
            state = local.state,
            observedQfHash = local.observedQfHash,
            stableQfHash = local.stableQfHash,
            stableSinceMs = local.stableSince,
            nowMs = clock.nowMs(),
            stabilizationMs = stabilizationMs,
            emulatorRunning = isEmulatorRunning(local.emulator),
        )

    private suspend fun <T> retryNetwork(operation: suspend () -> T): T {
        RETRY_DELAYS_MS.forEach { delayMs ->
            try {
                return operation()
            } catch (_: NetworkException) {
                sleeper.sleep(delayMs)
            }
        }
        return operation()
    }

    /**
     * M8 §7 : accorder les libellés, sans jamais écraser celui d'un humain.
     *
     * Deux mouvements. Adopter ce que le serveur annonce — c'est lui qui sait
     * qu'un autre appareil a renommé l'unité ou que la table 3DS connaît le
     * jeu. Puis proposer le nôtre s'il est meilleur : un client qui sait lire
     * le `PARAM.SFO` corrige le serial posé par un client plus ancien.
     *
     * Un libellé ne fait jamais échouer une passe (§10) : toute erreur laisse
     * simplement les noms en l'état.
     */
    private suspend fun reconcileLabels(remoteByIdentity: Map<Pair<String, String>, RemoteUnit>) {
        store.listUnits().forEach { local ->
            val remote = remoteByIdentity[local.emulator to local.unitKey] ?: return@forEach
            if (local.serverId == null) return@forEach
            if (remote.gameLabel == local.gameLabel && remote.labelSource == local.labelSource) {
                return@forEach
            }
            if (remote.labelSource == "user" || local.labelSource == "user") {
                store.adoptRemoteLabel(local.localId, remote.gameLabel, remote.labelSource)
                return@forEach
            }
            if (remote.gameLabel == local.gameLabel) return@forEach
            // Le serveur arbitre ; ici on ne fait que proposer, et on recopie
            // ce qu'il a retenu — qui n'est pas forcément ce qu'on a envoyé.
            val declared =
                runCatching { api.createUnit(unitRef(local), UUID.randomUUID().toString()) }.getOrNull()
                    ?: return@forEach
            store.adoptRemoteLabel(local.localId, declared.gameLabel, declared.labelSource)
        }
    }

    private suspend fun refresh(local: LocalUnit): LocalUnit =
        store.getUnit(local.emulator, local.unitKey)
            ?: error("local unit disappeared from the state store")

    private suspend fun recordError(
        report: SyncReport,
        message: String,
        localId: Long? = null,
    ) {
        report.errors += 1
        report.messages += message
        store.journal(clock.nowMs(), "error", message, localId)
    }

    private fun Exception.isExpectedSyncFailure(): Boolean =
        this is IllegalArgumentException ||
            this is IllegalStateException ||
            this is IOException ||
            this is SyncException ||
            this is UnsupportedNodeException ||
            this is RootInaccessibleException

    private companion object {
        // AD-30 : trois passages qui tuent le process sur la même tête
        // suffisent à décider que le poison vient de l'unité, pas du hasard.
        const val QUARANTINE_THRESHOLD = 3

        val NON_ACTIVE_STATES = setOf("missing", "paused", "unsupported", "duplicate")
        val RETRY_DELAYS_MS = listOf(1_000L, 4_000L, 15_000L)

        fun unitRef(local: LocalUnit) =
            UnitRef(
                emulator = local.emulator,
                unitKey = local.unitKey,
                unitType = local.unitType,
                gameKey = local.gameKey,
                gameLabel = local.gameLabel,
                root = RootRef(local.rootId),
                relPath = local.relPath,
            )

        fun clientMtime(nodes: List<NodeMeta>): String? =
            nodes.mapNotNull(NodeMeta::mtimeMs).maxOrNull()?.let {
                Instant.ofEpochMilli(it).toString()
            }

        fun sha256Hex(content: ByteArray): String =
            MessageDigest
                .getInstance("SHA-256")
                .digest(content)
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
