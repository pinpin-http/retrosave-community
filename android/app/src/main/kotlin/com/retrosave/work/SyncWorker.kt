package com.retrosave.work

import android.content.Context
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import com.retrosave.RetroSaveApp
import com.retrosave.sync.SyncTrigger
import com.retrosave.sync.SyncUiState
import java.util.concurrent.TimeUnit

class SyncWorker(
    appContext: Context,
    parameters: WorkerParameters,
) : CoroutineWorker(appContext, parameters) {
    override suspend fun doWork(): Result {
        val app = applicationContext as RetroSaveApp
        val container = app.container
        val settings = container.settings.snapshot()
        if (!settings.isConnected) {
            return Result.success()
        }

        // M7 §5, étage 1 : la passe périodique demande d'abord si une partie a
        // eu lieu depuis la dernière fois. La permission peut être retirée à
        // tout moment sans notification — d'où la question à chaque passe, et
        // une dégradation propre vers le périodique quand la réponse est non.
        //
        // `known` vient du disque, pas de la mémoire : ce worker est un objet
        // neuf à chaque réveil, et une partie plus longue qu'un cycle a son
        // début dans une fenêtre et sa fin dans une autre. C'est le cas nominal
        // d'une vraie soirée, et il ne déclenchait rien avant le 02/08.
        val now = System.currentTimeMillis()
        val window =
            container.usageSessions.read(
                packages = container.watchedPackages(),
                sinceMs = settings.lastUsageQueryMs,
                nowMs = now,
                known = settings.runningSessions,
            )
        if (window.permitted) {
            container.settings.saveSessionState(now, window.running)
        }
        // M7 §6 : une partie en cours au moment où on regarde. On avertit si le
        // cloud est en avance, et on n'écrit rien — le cas 6 du banc interdit
        // d'appliquer tant qu'un émulateur tourne, et c'est justement pour que
        // le joueur ferme le jeu avant que la passe s'en occupe.
        if (window.running.isNotEmpty()) {
            container.warnIfCloudAhead(window.running)
        }

        val trigger = if (window.sawSession) SyncTrigger.SESSION_ENDED else SyncTrigger.PERIODIC

        return when (val state = container.syncService.runPass(trigger)) {
            is SyncUiState.Completed ->
                if (state.report.retryable) Result.retry() else Result.success()
            is SyncUiState.Failed ->
                if (state.retryable) Result.retry() else Result.failure()
            SyncUiState.Idle,
            is SyncUiState.Running,
            -> Result.failure()
        }
    }

    companion object {
        private const val UNIQUE_WORK_NAME = "retrosave-sync"

        fun schedule(context: Context) {
            val constraints =
                Constraints
                    .Builder()
                    .setRequiredNetworkType(NetworkType.CONNECTED)
                    .build()
            val request =
                PeriodicWorkRequestBuilder<SyncWorker>(15, TimeUnit.MINUTES)
                    .setConstraints(constraints)
                    .setBackoffCriteria(
                        BackoffPolicy.EXPONENTIAL,
                        30,
                        TimeUnit.SECONDS,
                    ).build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                UNIQUE_WORK_NAME,
                ExistingPeriodicWorkPolicy.KEEP,
                request,
            )
        }
    }
}
