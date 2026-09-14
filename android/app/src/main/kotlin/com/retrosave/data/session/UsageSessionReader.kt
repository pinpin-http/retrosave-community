package com.retrosave.data.session

import android.app.AppOpsManager
import android.app.usage.UsageEvents
import android.app.usage.UsageStatsManager
import android.content.Context
import android.os.Process
import com.retrosave.core.session.UsageEventRecord
import com.retrosave.core.session.UsageEventType
import com.retrosave.core.session.foldUsageEvents
import kotlin.math.max
import kotlin.math.min

/**
 * Lecture des sessions d'émulateur via `UsageStatsManager` (M7 §5, étage 1).
 *
 * Voie unique et légale : `queryEvents()` avec la permission spéciale
 * `PACKAGE_USAGE_STATS`. `getRunningAppProcesses()` ne rend que le process
 * courant depuis Android 5 et ne sert donc à rien ici.
 *
 * AD-33 : ce lecteur dit *qu'une partie a eu lieu*, jamais ce que la passe doit
 * décider. Rien ne sort de l'appareil, et seuls les packages des adaptateurs
 * configurés sont regardés — pas l'usage général du téléphone.
 *
 * La décision elle-même est dans [foldUsageEvents], pure et testée : elle a été
 * fausse une fois, et elle l'était de façon invisible depuis le PC.
 */
class UsageSessionReader(
    private val context: Context,
) {
    /**
     * La permission peut être retirée à tout moment, sans notification.
     *
     * D'où la vérification à chaque passe plutôt qu'une fois au démarrage : un
     * appel sans permission ne lève pas, il rend simplement zéro événement — et
     * on conclurait « aucune partie » au lieu de « je ne sais pas ».
     */
    fun hasPermission(): Boolean {
        val appOps =
            context.getSystemService(Context.APP_OPS_SERVICE) as? AppOpsManager
                ?: return false
        val mode =
            appOps.unsafeCheckOpNoThrow(
                AppOpsManager.OPSTR_GET_USAGE_STATS,
                Process.myUid(),
                context.packageName,
            )
        return mode == AppOpsManager.MODE_ALLOWED
    }

    /**
     * Rendre les packages surveillés dont une session s'est **terminée** depuis
     * `sinceMs`, et ceux dont une session est **encore en cours**.
     *
     * @param known état déjà connu avant cette fenêtre. Un appelant qui sonde
     *   souvent (le service d'étage 2) passe ce qu'il sait et n'interroge que
     *   les changements ; sans cela il faudrait redemander l'historique complet
     *   toutes les vingt-cinq secondes, ce que la batterie paierait.
     * @param lookbackMs profondeur de la fenêtre. Elle doit remonter assez loin
     *   pour contenir le dernier événement de chaque package surveillé : c'est
     *   cette profondeur qui autorise à traiter un `known` sans écho comme
     *   périmé plutôt que comme une partie éternelle.
     */
    fun read(
        packages: Set<String>,
        sinceMs: Long,
        nowMs: Long,
        known: Set<String> = emptySet(),
        lookbackMs: Long = MAX_WINDOW_MS,
    ): UsageWindow {
        if (packages.isEmpty() || !hasPermission()) {
            return UsageWindow(ended = emptySet(), running = known, permitted = hasPermission())
        }
        val manager =
            context.getSystemService(Context.USAGE_STATS_SERVICE) as? UsageStatsManager
                ?: return UsageWindow(emptySet(), known, permitted = false)

        // Fenêtre plafonnée : au premier lancement le curseur est à zéro, et
        // demander l'historique complet coûte cher pour une information dont on
        // n'a que faire — une partie d'avant-hier ne motive aucune passe.
        // Le plancher `nowMs - lookbackMs` compte autant que le curseur : une
        // partie en cours n'émet plus rien après son lancement, et une fenêtre
        // qui commence après lui la rend invisible (constaté sur le Thor).
        val floor = max(0L, nowMs - lookbackMs)
        val from = min(floor, max(0L, min(sinceMs, nowMs)))
        val events = manager.queryEvents(from, nowMs)

        val collected = mutableListOf<UsageEventRecord>()
        val event = UsageEvents.Event()
        while (events.hasNextEvent()) {
            events.getNextEvent(event)
            val pkg = event.packageName
            if (pkg !in packages) continue
            val type = retainedType(event.eventType) ?: continue
            collected += UsageEventRecord(pkg, type, event.timeStamp)
        }

        // `corroborating` : la fenêtre remonte toujours au moins `lookbackMs`, donc
        // un package que le système n'y mentionne pas n'a plus d'état lisible —
        // et un « en cours » persisté qu'on garderait indéfiniment empêcherait
        // toute passe de fin de partie pour cette unité.
        val folded =
            foldUsageEvents(
                known = known,
                events = collected,
                sinceMs = sinceMs,
                nowMs = nowMs,
                corroborating = true,
            )
        return UsageWindow(ended = folded.ended, running = folded.running, permitted = true)
    }

    /**
     * Traduire un type de la plateforme vers le noyau pur.
     *
     * La correspondance est explicite plutôt qu'implicite par égalité de
     * nombres : le noyau ne doit pas dépendre du fait que les constantes
     * d'Android portent aujourd'hui ces valeurs-là.
     */
    private fun retainedType(eventType: Int): Int? =
        when (eventType) {
            UsageEvents.Event.ACTIVITY_RESUMED -> UsageEventType.ACTIVITY_RESUMED
            UsageEvents.Event.ACTIVITY_PAUSED -> UsageEventType.ACTIVITY_PAUSED
            UsageEvents.Event.ACTIVITY_STOPPED -> UsageEventType.ACTIVITY_STOPPED
            else -> null
        }

    private companion object {
        const val MAX_WINDOW_MS = 24L * 60 * 60 * 1000
    }
}

/**
 * Ce que la fenêtre d'événements a montré.
 *
 * `permitted` est distinct d'un ensemble vide : « aucune partie » et « je n'ai
 * pas le droit de regarder » demandent des comportements opposés — le premier
 * autorise une passe allégée, le second impose la dégradation vers le
 * périodique et un bandeau qui le dit.
 */
data class UsageWindow(
    val ended: Set<String>,
    val running: Set<String>,
    val permitted: Boolean,
) {
    val sawSession: Boolean
        get() = ended.isNotEmpty()
}
