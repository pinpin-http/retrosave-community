package com.retrosave.core.session

// Pliage des événements d'usage en états de session (AD-33).
//
// Pur et sans Android : c'est la partie qui décide, et elle a été fausse pour
// une seule raison — elle appariait des événements *à l'intérieur* d'une fenêtre
// au lieu de lire l'**état courant** d'un package.
//
// Mesuré sur le Thor le 02/08 : une partie lancée à 22:40:27 n'émet plus rien
// ensuite. Toute fenêtre ouverte après 22:40:27 ne contient aucun événement, et
// l'appariement en concluait « aucune partie en cours » alors que le jeu
// tournait depuis quatre minutes. L'état d'un package se lit sur son dernier
// événement, pas sur ce qu'une fenêtre arbitraire a bien voulu capturer.

/**
 * Types d'événements retenus, repris de `UsageEvents.Event` sans en dépendre.
 *
 * La correspondance est établie côté Android, pas par égalité de nombres.
 */
object UsageEventType {
    const val ACTIVITY_RESUMED = 1
    const val ACTIVITY_PAUSED = 2
    const val ACTIVITY_STOPPED = 23
}

data class UsageEventRecord(
    val packageName: String,
    val type: Int,
    val timestampMs: Long,
)

/**
 * Rendre l'état des packages surveillés après application des événements.
 *
 * @param known packages retenus comme en cours avant cette fenêtre. Cet état est
 *   **persisté** : un `SyncWorker` est un objet neuf à chaque réveil, et le
 *   process peut mourir entre deux cycles. Le garder en mémoire seule reviendrait
 *   à repartir de zéro précisément quand la partie a été longue.
 * @param sinceMs curseur : une fin de session n'est signalée que si elle est
 *   postérieure, sinon on rejouerait la même partie à chaque passe.
 * @param nowMs instant de la lecture, daté des fins de session déduites.
 * @param corroborating vrai quand la fenêtre remonte assez loin pour contenir le
 *   dernier événement de chaque package. Alors, et alors seulement, un `known`
 *   sans aucun événement est un état **périmé** plutôt qu'une partie en cours.
 */
fun foldUsageEvents(
    known: Set<String>,
    events: List<UsageEventRecord>,
    sinceMs: Long,
    nowMs: Long = Long.MAX_VALUE,
    corroborating: Boolean = false,
): UsageFoldResult {
    val running = known.toMutableSet()
    val endedAt = mutableMapOf<String, Long>()
    val seen = mutableSetOf<String>()

    // Les événements arrivent triés par le fournisseur, mais rien ne l'impose au
    // contrat : trier ici coûte peu et évite qu'un fournisseur bavard renverse
    // l'état d'un package pour une raison d'implémentation.
    events.sortedBy { it.timestampMs }.forEach { event ->
        seen += event.packageName
        when (event.type) {
            UsageEventType.ACTIVITY_RESUMED -> {
                running += event.packageName
                endedAt -= event.packageName
            }
            // ACTIVITY_STOPPED compte autant que PAUSED : un émulateur tué par
            // l'utilisateur ou par le système n'émet jamais de PAUSED, et une
            // partie ainsi terminée resterait « en cours » indéfiniment.
            UsageEventType.ACTIVITY_PAUSED, UsageEventType.ACTIVITY_STOPPED -> {
                running -= event.packageName
                endedAt[event.packageName] = event.timestampMs
            }
        }
    }

    if (corroborating) {
        // Un état persisté que même une fenêtre profonde ne confirme pas est
        // périmé : le process a disparu sans laisser d'événement lisible, ou les
        // événements ont vieilli hors de la fenêtre. Le croire indéfiniment
        // condamnerait l'unité à ne plus jamais être poussée à la fin d'une
        // partie — un état bloqué est pire qu'un déclenchement de trop.
        known.filterNot { it in seen }.forEach { stale ->
            running -= stale
            endedAt[stale] = nowMs
        }
    }

    return UsageFoldResult(
        running = running.toSet(),
        ended = endedAt.filterValues { it > sinceMs }.keys.toSet(),
    )
}

data class UsageFoldResult(
    val running: Set<String>,
    val ended: Set<String>,
)
