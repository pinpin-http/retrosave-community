package com.retrosave.data.db

import androidx.room.withTransaction
import com.retrosave.core.model.LocalUnit
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.UnitRef
import com.retrosave.core.sync.UnitStore
import com.retrosave.data.db.entities.FileFingerprintEntity
import com.retrosave.data.db.entities.JournalEntity
import com.retrosave.data.db.entities.UnitEntity

class RoomUnitStore(
    private val db: AppDatabase,
) : UnitStore {
    override suspend fun listUnits(emulator: String?): List<LocalUnit> =
        (emulator?.let { db.units().listByEmulator(it) } ?: db.units().listAll())
            .map { it.toLocal() }

    override suspend fun getUnit(
        emulator: String,
        unitKey: String,
    ): LocalUnit? = db.units().find(emulator, unitKey)?.toLocal()

    override suspend fun getUnitByServerId(serverId: String): LocalUnit? = db.units().findByServerId(serverId)?.toLocal()

    override suspend fun upsertScan(
        unit: UnitRef,
        qfHash: String,
        nodes: List<NodeMeta>,
        now: Long,
    ): Pair<LocalUnit, Boolean> =
        db.withTransaction {
            val existing = db.units().find(unit.emulator, unit.unitKey)
            val changed = existing == null || existing.observedQfHash != qfHash
            val localId =
                if (existing == null) {
                    db.units().insert(
                        UnitEntity(
                            emulator = unit.emulator,
                            unitKey = unit.unitKey,
                            unitType = unit.unitType,
                            gameKey = unit.gameKey,
                            gameLabel = unit.gameLabel,
                            rootId = unit.root.rootId,
                            relPath = unit.relPath,
                            observedQfHash = qfHash,
                            observedAt = now,
                            stableQfHash = qfHash,
                            stableSince = now,
                        ),
                    )
                } else {
                    db.units().update(
                        existing.copy(
                            unitType = unit.unitType,
                            gameKey = unit.gameKey,
                            // M8 §7 : le scan écrasait le libellé à chaque
                            // découverte. Sur une unité renommée à la main, la
                            // liste serait retombée au serial à la passe
                            // suivante, pendant que le serveur gardait le vrai
                            // nom — deux appareils, deux noms.
                            gameLabel =
                                if (existing.labelSource == "user") {
                                    existing.gameLabel
                                } else {
                                    unit.gameLabel
                                },
                            rootId = unit.root.rootId,
                            relPath = unit.relPath,
                            observedQfHash = if (changed) qfHash else existing.observedQfHash,
                            observedAt = if (changed) now else existing.observedAt,
                            stableQfHash = if (changed) qfHash else existing.stableQfHash,
                            stableSince = if (changed) now else existing.stableSince,
                            state = "active",
                        ),
                    )
                    existing.localId
                }
            replaceFingerprints(localId, nodes)
            requireNotNull(db.units().findByLocalId(localId)).toLocal() to changed
        }

    override suspend fun addRemotePlaceholder(
        unit: UnitRef,
        serverId: String,
    ): LocalUnit =
        db.withTransaction {
            val existing = db.units().find(unit.emulator, unit.unitKey)
            val localId =
                if (existing == null) {
                    db.units().insert(
                        UnitEntity(
                            serverId = serverId,
                            emulator = unit.emulator,
                            unitKey = unit.unitKey,
                            unitType = unit.unitType,
                            gameKey = unit.gameKey,
                            gameLabel = unit.gameLabel,
                            rootId = unit.root.rootId,
                            relPath = unit.relPath,
                        ),
                    )
                } else {
                    db.units().update(
                        existing.copy(
                            serverId = serverId,
                            gameLabel =
                                if (existing.labelSource == "user") {
                                    existing.gameLabel
                                } else {
                                    unit.gameLabel
                                },
                        ),
                    )
                    existing.localId
                }
            requireNotNull(db.units().findByLocalId(localId)).toLocal()
        }

    override suspend fun markMissingExcept(
        emulator: String,
        rootId: String,
        discoveredKeys: Set<String>,
    ): List<LocalUnit> =
        db.withTransaction {
            val missing =
                db
                    .units()
                    .listByEmulator(emulator)
                    .filter {
                        it.rootId == rootId &&
                            it.unitKey !in discoveredKeys &&
                            it.state == "active" &&
                            it.observedQfHash != null
                    }
            missing.forEach { db.units().update(it.copy(state = "missing")) }
            missing.map { it.toLocal() }
        }

    override suspend fun bindServer(
        localId: Long,
        serverId: String,
    ) {
        update(localId) { it.copy(serverId = serverId) }
    }

    override suspend fun markUnsupported(localId: Long) {
        update(localId) { it.copy(state = "unsupported") }
    }

    override suspend fun markDuplicate(localId: Long) {
        update(localId) { it.copy(state = "duplicate") }
    }

    override suspend fun pauseAll() {
        db.units().pauseAll()
    }

    override suspend fun resumeAll() {
        db.units().resumeAll()
    }

    override suspend fun markError(localId: Long) {
        update(localId) { it.copy(state = "error") }
    }

    override suspend fun recordFailure(
        localId: Long,
        headVersion: Int?,
    ) {
        update(localId) {
            it.copy(
                consecutiveFailures = it.consecutiveFailures + 1,
                lastFailureHead = headVersion,
            )
        }
    }

    override suspend fun clearFailures(localId: Long) {
        update(localId) { it.copy(consecutiveFailures = 0, lastFailureHead = null) }
    }

    override suspend fun resumeUnit(localId: Long) {
        update(localId) {
            if (it.state == "error" || it.state == "paused") it.copy(state = "active") else it
        }
    }

    override suspend fun setSynced(
        localId: Long,
        version: Int,
        contentSha256: String,
        qfHash: String?,
        now: Long?,
        clearPending: Boolean,
    ) {
        update(localId) {
            it.copy(
                lastSyncedVersion = version,
                lastSyncedContentSha256 = contentSha256,
                state = "active",
                openConflictId = null,
                observedQfHash = qfHash ?: it.observedQfHash,
                observedAt = if (qfHash != null) now else it.observedAt,
                stableQfHash = qfHash ?: it.stableQfHash,
                stableSince = if (qfHash != null) now else it.stableSince,
                pendingBranchNumber =
                    if (clearPending) null else it.pendingBranchNumber,
                pendingBranchContentSha256 =
                    if (clearPending) null else it.pendingBranchContentSha256,
                // AD-27 : le marqueur tombe avec lastSynced, jamais avant lui.
                applyJournalVersion = null,
                applyJournalContentSha256 = null,
                applyJournalStartedAt = null,
                // AD-30 : un succès désarme la quarantaine.
                consecutiveFailures = 0,
                lastFailureHead = null,
            )
        }
    }

    override suspend fun setConflict(
        localId: Long,
        conflictId: String,
        pendingBranchNumber: Int?,
        pendingBranchContentSha256: String?,
    ) {
        update(localId) {
            it.copy(
                openConflictId = conflictId,
                pendingBranchNumber = pendingBranchNumber,
                pendingBranchContentSha256 = pendingBranchContentSha256,
            )
        }
    }

    override suspend fun clearOpenConflict(localId: Long) {
        update(localId) { it.copy(openConflictId = null) }
    }

    override suspend fun setApplyJournal(
        localId: Long,
        version: Int,
        contentSha256: String,
        now: Long,
    ) {
        update(localId) {
            it.copy(
                applyJournalVersion = version,
                applyJournalContentSha256 = contentSha256,
                applyJournalStartedAt = now,
            )
        }
    }

    override suspend fun clearApplyJournal(localId: Long) {
        update(localId) {
            it.copy(
                applyJournalVersion = null,
                applyJournalContentSha256 = null,
                applyJournalStartedAt = null,
            )
        }
    }

    override suspend fun fingerprints(localId: Long): List<NodeMeta> =
        db.fingerprints().listForUnit(localId).map {
            NodeMeta(
                relPath = it.relPath,
                sizeBytes = it.sizeBytes,
                mtimeMs = it.mtimeMs,
            )
        }

    override suspend fun journal(
        now: Long,
        event: String,
        detail: String,
        localId: Long?,
    ) {
        db.withTransaction {
            db.journal().insert(
                JournalEntity(
                    ts = now,
                    unitLocalId = localId,
                    event = event,
                    detail = detail,
                ),
            )
            db.journal().trim()
        }
    }

    private suspend fun replaceFingerprints(
        localId: Long,
        nodes: List<NodeMeta>,
    ) {
        db.fingerprints().deleteForUnit(localId)
        if (nodes.isNotEmpty()) {
            db.fingerprints().insertAll(
                nodes.map {
                    FileFingerprintEntity(
                        unitLocalId = localId,
                        relPath = it.relPath,
                        sizeBytes = it.sizeBytes,
                        mtimeMs = it.mtimeMs,
                    )
                },
            )
        }
    }

    private suspend fun update(
        localId: Long,
        transform: (UnitEntity) -> UnitEntity,
    ) {
        db.withTransaction {
            val existing =
                db.units().findByLocalId(localId)
                    ?: error("Unknown local unit $localId")
            db.units().update(transform(existing))
        }
    }

    override suspend fun adoptRemoteLabel(
        localId: Long,
        label: String,
        source: String,
    ) {
        update(localId) { it.copy(gameLabel = label, labelSource = source) }
    }

    private fun UnitEntity.toLocal() =
        LocalUnit(
            localId = localId,
            serverId = serverId,
            emulator = emulator,
            unitKey = unitKey,
            unitType = unitType,
            gameKey = gameKey,
            gameLabel = gameLabel,
            labelSource = labelSource,
            rootId = rootId,
            relPath = relPath,
            observedQfHash = observedQfHash,
            observedAt = observedAt,
            stableQfHash = stableQfHash,
            stableSince = stableSince,
            lastSyncedVersion = lastSyncedVersion,
            lastSyncedContentSha256 = lastSyncedContentSha256,
            state = state,
            openConflictId = openConflictId,
            pendingBranchNumber = pendingBranchNumber,
            pendingBranchContentSha256 = pendingBranchContentSha256,
            applyJournalVersion = applyJournalVersion,
            applyJournalContentSha256 = applyJournalContentSha256,
            applyJournalStartedAt = applyJournalStartedAt,
        )
}
