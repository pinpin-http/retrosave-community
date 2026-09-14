package com.retrosave.core.sync

// Fraîcheur vérifiée — AD-36 (Q32), partie pure et partagée.
//
// « À jour » sans date veut dire « je n'ai rien à envoyer », pas « j'ai
// vérifié ». La nuance est invisible tant que tout va bien, et c'est toute la
// différence le jour où le déclencheur est mort : l'écran continue d'afficher
// exactement la même chose.
//
// L'état « stopped » d'Android n'est qu'un membre d'une famille — gestionnaire
// de batterie, Doze, permission retirée, quota, appareil éteint, wifi absent.
// Aucune n'est réparable côté code ; toutes mentent de la même façon. On traite
// la classe, pas le cas.

/** Une passe automatique plus vieille que ça mérite d'être signalée. */
const val STALE_PERIODIC_MS: Long = 24L * 60 * 60 * 1000

/** Un appareil muet plus longtemps que ça mérite un point d'attention. */
const val STALE_DEVICE_MS: Long = 48L * 60 * 60 * 1000

/**
 * Faut-il alerter sur la synchronisation automatique ?
 *
 * On regarde la passe **périodique**, jamais celle à l'ouverture : la seconde
 * remet tout à jour au moment précis où l'utilisateur regarde, et masque donc
 * exactement la panne qu'on cherche à rendre visible.
 */
fun periodicSyncIsStale(
    lastPeriodicSuccessAtMs: Long,
    nowMs: Long,
): Boolean = lastPeriodicSuccessAtMs <= 0L || nowMs - lastPeriodicSuccessAtMs > STALE_PERIODIC_MS

/** Un appareil silencieux depuis assez longtemps pour valoir un signalement. */
fun deviceIsQuiet(
    lastSeenAtMs: Long?,
    nowMs: Long,
): Boolean = lastSeenAtMs == null || nowMs - lastSeenAtMs > STALE_DEVICE_MS
