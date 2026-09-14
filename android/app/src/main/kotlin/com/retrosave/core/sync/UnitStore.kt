package com.retrosave.core.sync

import com.retrosave.core.model.LocalUnit
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.UnitRef

/** Port de persistance exact consommé par le moteur pur. */
interface UnitStore {
    suspend fun listUnits(emulator: String? = null): List<LocalUnit>

    suspend fun getUnit(
        emulator: String,
        unitKey: String,
    ): LocalUnit?

    suspend fun getUnitByServerId(serverId: String): LocalUnit?

    suspend fun upsertScan(
        unit: UnitRef,
        qfHash: String,
        nodes: List<NodeMeta>,
        now: Long,
    ): Pair<LocalUnit, Boolean>

    suspend fun addRemotePlaceholder(
        unit: UnitRef,
        serverId: String,
    ): LocalUnit

    suspend fun markMissingExcept(
        emulator: String,
        rootId: String,
        discoveredKeys: Set<String>,
    ): List<LocalUnit>

    suspend fun bindServer(
        localId: Long,
        serverId: String,
    )

    suspend fun markUnsupported(localId: Long)

    suspend fun markDuplicate(localId: Long)

    /** AD-28/AD-30 : une unité mise de côté, sans toucher aux autres. */
    suspend fun markError(localId: Long)

    suspend fun pauseAll()

    suspend fun resumeAll()

    /** AD-30 : compter avant de traiter, pour survivre à ce qui tue le process. */
    suspend fun recordFailure(
        localId: Long,
        headVersion: Int?,
    )

    /** Une unité qui aboutit repart de zéro. */
    suspend fun clearFailures(localId: Long)

    /**
     * Recopier le libellé arbitré par le serveur dans le miroir local (M8 §7).
     *
     * Le serveur est l'arbitre : lui seul sait qu'un autre appareil a renommé
     * l'unité à la main, ou que la table 3DS connaît le jeu. Sans cette
     * recopie, deux appareils afficheraient deux noms pour la même sauvegarde.
     */
    suspend fun adoptRemoteLabel(
        localId: Long,
        label: String,
        source: String,
    )

    /** Remettre une seule unité en jeu après quarantaine. */
    suspend fun resumeUnit(localId: Long)

    suspend fun setSynced(
        localId: Long,
        version: Int,
        contentSha256: String,
        qfHash: String? = null,
        now: Long? = null,
        clearPending: Boolean = false,
    )

    suspend fun setConflict(
        localId: Long,
        conflictId: String,
        pendingBranchNumber: Int? = null,
        pendingBranchContentSha256: String? = null,
    )

    suspend fun clearOpenConflict(localId: Long)

    suspend fun setApplyJournal(
        localId: Long,
        version: Int,
        contentSha256: String,
        now: Long,
    )

    suspend fun clearApplyJournal(localId: Long)

    suspend fun fingerprints(localId: Long): List<NodeMeta>

    suspend fun journal(
        now: Long,
        event: String,
        detail: String,
        localId: Long? = null,
    )
}
