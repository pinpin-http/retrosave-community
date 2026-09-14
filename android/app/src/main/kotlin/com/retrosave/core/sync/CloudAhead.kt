package com.retrosave.core.sync

import com.retrosave.core.model.LocalUnit

/**
 * Règle « début de session » : avertir, jamais écrire (M7 §6).
 *
 * Miroir exact de `cloud_ahead.py`. Pure : elle compare ce que le serveur
 * annonce à ce que la comptabilité locale retient, et ne sait pas ce qu'est un
 * émulateur.
 *
 * C'est la moitié qui manquait à la synchronisation post-partie. Sans elle, on
 * améliore l'envoi et on continue de fabriquer des conflits à la reprise : le
 * joueur lance son jeu sur une sauvegarde périmée, joue une heure, et découvre
 * le problème au moment de la fusion.
 *
 * Le cas 6 du banc reste intact : cette fonction ne rend que des noms à
 * afficher, rien n'est appliqué tant qu'un émulateur tourne.
 */
fun unitsBehindCloud(
    emulator: String,
    remoteUnits: List<RemoteUnit>,
    localUnits: List<LocalUnit>,
): List<String> {
    val localByKey =
        localUnits.filter { it.emulator == emulator }.associateBy { it.unitKey }
    return remoteUnits
        .asSequence()
        .filter { it.emulator == emulator && it.state == "active" }
        .mapNotNull { remote ->
            // Une unité absente localement n'est pas en retard : elle est à
            // placer ou non configurée (§9.6). Prévenir « une sauvegarde plus
            // récente existe » enverrait le joueur fermer son jeu pour un
            // problème qui n'est pas celui-là.
            val local = localByKey[remote.unitKey] ?: return@mapNotNull null
            remote.unitKey.takeIf { remote.headVersion > local.lastSyncedVersion }
        }.sorted()
        .toList()
}
