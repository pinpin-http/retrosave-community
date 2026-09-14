package com.retrosave.sync

import com.retrosave.core.model.SyncReport
import com.retrosave.core.sync.NetworkException
import com.retrosave.core.sync.SyncApi
import com.retrosave.core.sync.SyncEngine
import com.retrosave.data.icons.ArtworkFetcher
import com.retrosave.data.settings.SettingsStore
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.util.UUID

enum class SyncTrigger {
    OPEN,
    MANUAL,
    PERIODIC,

    /** M7 §5 : une partie s'est terminée depuis la dernière passe (AD-33). */
    SESSION_ENDED,
}

sealed interface SyncUiState {
    data object Idle : SyncUiState

    data class Running(
        val trigger: SyncTrigger,
    ) : SyncUiState

    data class Completed(
        val trigger: SyncTrigger,
        val report: SyncReport,
    ) : SyncUiState

    data class Failed(
        val trigger: SyncTrigger,
        val message: String,
        val retryable: Boolean,
    ) : SyncUiState
}

class SyncService(
    private val engine: SyncEngine,
    private val api: SyncApi,
    private val settings: SettingsStore? = null,
    // Q45 : facultatif. Sans lui, la bibliothèque garde ses pastilles de
    // repli — c'est une dégradation visuelle, jamais un défaut de
    // synchronisation.
    private val artwork: ArtworkFetcher? = null,
) {
    private val passMutex = Mutex()
    private val _state = MutableStateFlow<SyncUiState>(SyncUiState.Idle)
    val state: StateFlow<SyncUiState> = _state.asStateFlow()

    suspend fun runPass(trigger: SyncTrigger): SyncUiState =
        passMutex.withLock {
            _state.value = SyncUiState.Running(trigger)
            val result =
                try {
                    SyncUiState.Completed(trigger, engine.sync())
                } catch (error: CancellationException) {
                    throw error
                } catch (error: NetworkException) {
                    SyncUiState.Failed(
                        trigger = trigger,
                        message = error.message ?: "Network unreachable.",
                        retryable = true,
                    )
                } catch (error: Exception) {
                    SyncUiState.Failed(
                        trigger = trigger,
                        message = error.message ?: "Sync failed.",
                        retryable = false,
                    )
                }
            // AD-36 : un seul point de passage, pour qu'aucun déclencheur
            // n'oublie de dater sa réussite. Une passe avec erreurs n'a pas
            // constaté la fraîcheur : elle a échoué à la constater.
            if (result is SyncUiState.Completed && result.report.errors == 0) {
                settings?.recordSuccessfulPass(System.currentTimeMillis(), trigger.name)
            }
            // Les jaquettes, APRÈS la synchronisation et hors de son filet :
            // rien ici ne peut changer le résultat d'une passe, et un catalogue
            // injoignable ne coûte qu'une pastille de repli.
            if (artwork != null && result is SyncUiState.Completed) {
                try {
                    artwork.refresh(api.listUnits())
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Exception) {
                    // Silencieux par construction.
                }
            }
            _state.value = result
            result
        }

    /**
     * Tranche un conflit, puis applique la version gagnante localement.
     *
     * La branche perdante reste dans l'historique serveur : rien n'est
     * supprimé, conformément à I3.
     */
    suspend fun resolveConflict(
        conflictId: String,
        winner: Int,
    ): SyncUiState =
        passMutex.withLock {
            _state.value = SyncUiState.Running(SyncTrigger.MANUAL)
            val result =
                try {
                    api.resolveConflict(
                        conflictId = conflictId,
                        winner = winner,
                        idempotencyKey = UUID.randomUUID().toString(),
                    )
                    // Une passe complète, comme `rsc resolve` : c'est elle qui
                    // oublie le conflit résolu dans le miroir local avant
                    // d'appliquer le gagnant. Un pull direct serait refusé tant
                    // que `openConflictId` est encore posé.
                    SyncUiState.Completed(SyncTrigger.MANUAL, engine.sync())
                } catch (error: CancellationException) {
                    throw error
                } catch (error: NetworkException) {
                    SyncUiState.Failed(
                        trigger = SyncTrigger.MANUAL,
                        message = error.message ?: "Network unreachable.",
                        retryable = true,
                    )
                } catch (error: Exception) {
                    SyncUiState.Failed(
                        trigger = SyncTrigger.MANUAL,
                        message = error.message ?: "Resolving failed.",
                        retryable = false,
                    )
                }
            _state.value = result
            result
        }

    suspend fun restore(
        unitId: String,
        version: Int,
    ): SyncUiState =
        passMutex.withLock {
            _state.value = SyncUiState.Running(SyncTrigger.MANUAL)
            val result =
                try {
                    api.restore(
                        unitId = unitId,
                        number = version,
                        idempotencyKey = UUID.randomUUID().toString(),
                    )
                    val remote =
                        api.listUnits().singleOrNull { it.id == unitId }
                            ?: error("Restored save not found on the server.")
                    val report = SyncReport()
                    engine.pullRemoteUnit(remote, report)
                    SyncUiState.Completed(SyncTrigger.MANUAL, report)
                } catch (error: CancellationException) {
                    throw error
                } catch (error: NetworkException) {
                    SyncUiState.Failed(
                        trigger = SyncTrigger.MANUAL,
                        message = error.message ?: "Network unreachable.",
                        retryable = true,
                    )
                } catch (error: Exception) {
                    SyncUiState.Failed(
                        trigger = SyncTrigger.MANUAL,
                        message = error.message ?: "Restore failed.",
                        retryable = false,
                    )
                }
            _state.value = result
            result
        }
}
