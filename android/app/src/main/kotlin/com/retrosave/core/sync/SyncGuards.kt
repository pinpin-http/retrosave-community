package com.retrosave.core.sync

fun isStable(
    state: String,
    observedQfHash: String?,
    stableQfHash: String?,
    stableSinceMs: Long?,
    nowMs: Long,
    stabilizationMs: Long,
    emulatorRunning: Boolean,
): Boolean =
    state == "active" &&
        observedQfHash != null &&
        observedQfHash == stableQfHash &&
        stableSinceMs != null &&
        nowMs - stableSinceMs >= stabilizationMs &&
        !emulatorRunning

fun shouldPauseForMissing(
    missingCount: Int,
    threshold: Int = 10,
): Boolean = missingCount >= threshold

fun quarantineState(relPaths: List<String>): String = if (relPaths.toSet().size > 1) "duplicate" else "active"

/**
 * AD-30 : que faire d'une unité qui a déjà fait tomber la passe ?
 *
 * Un `catch` ne suffit pas — un crash natif de zstd-jni ou un kill système
 * n'exécutent aucun bloc de reprise. Le compteur est donc écrit *avant* le
 * traitement, et cette fonction dit ce qu'il faut faire de ce qu'on relit au
 * tour suivant.
 *
 * Trois issues, jamais deux :
 *
 * - `proceed` — sous le seuil : l'unité passe normalement ;
 * - `rearm` — au seuil, mais la tête serveur a **changé**. Le poison est le
 *   plus souvent lié à un contenu précis, donc on repart de zéro ;
 * - `quarantine` — au seuil sur la **même** tête : l'unité est écartée, la
 *   passe et l'application restent utilisables.
 *
 * Sans la troisième issue, une seule unité empoisonnée fait tomber le process
 * à chaque passe, indéfiniment, et plus rien ne se synchronise.
 *
 * Le fichier `tests/vectors/quarantine_decision.json` est rejoué à l'identique
 * par Python et par le noyau C++ : les trois implémentations doivent trancher
 * pareil.
 */
fun quarantineDecision(
    consecutiveFailures: Int,
    lastFailureHead: Int?,
    headVersion: Int?,
    threshold: Int = 3,
): String =
    when {
        consecutiveFailures < threshold -> "proceed"
        // Comparer deux `null` les rend égaux : une unité qui a échoué trois
        // fois avant d'exister sur le serveur reste écartée, et c'est voulu.
        lastFailureHead != headVersion -> "rearm"
        else -> "quarantine"
    }

// Cette garde ne remplace pas la détection : Android ne sait pas toujours
// observer les autres applications. Un résultat actif interdit la réception.
fun mayApplyWithEmulator(emulatorRunning: Boolean): Boolean = !emulatorRunning

/**
 * SYN-06 et SYN-07 : cet appareil doit-il s'occuper de cette sauvegarde ?
 *
 * Deux demandes distinctes côté produit — mettre en pause, et choisir les jeux
 * suivis sur *cet* appareil — mais une seule règle, pour qu'elles ne puissent
 * pas diverger. Trois issues :
 *
 * - `sync` — oui, comme d'habitude ;
 * - `skip_paused` — non, l'utilisateur a mis en pause (ce jeu, ou tout) ;
 * - `skip_excluded` — non, ce jeu n'est pas sélectionné sur cet appareil.
 *
 * Rien n'est envoyé au serveur, rien n'est supprimé : un autre appareil
 * continue de synchroniser la même sauvegarde sans rien savoir de ce choix.
 *
 * L'exclusion est testée en premier : elle doit survivre à la levée d'une
 * pause générale. Et une valeur inconnue **synchronise** — un réglage illisible
 * ne doit jamais arrêter la protection en silence, alors que synchroniser
 * n'est jamais destructeur.
 *
 * Vecteur partagé : `tests/vectors/local_sync_mode.json`, rejoué à l'identique
 * par le noyau C++.
 */
fun localSyncDecision(
    mode: String,
    globalPause: Boolean,
): String =
    when {
        mode == "excluded" -> "skip_excluded"
        globalPause || mode == "paused" -> "skip_paused"
        else -> "sync"
    }

/**
 * Un libellé est « brut » quand il n'est qu'un morceau de l'identifiant.
 *
 * Azahar pose le `titleid_low`, qui **est** la clé d'unité ; PPSSPP pose le
 * serial, qui en est le préfixe. Un vrai titre — « LocoRoco » pour
 * `UCET00357_GameData0` — n'en est pas un préfixe.
 */
fun isRawLabel(
    label: String,
    unitKey: String,
): Boolean = label.isNotEmpty() && unitKey.startsWith(label)

/**
 * M8 §7 : faut-il adopter le libellé que le serveur nous renvoie ?
 *
 * Le serveur résout le vrai titre d'un jeu 3DS à partir de son `titleid_low` ;
 * un client qui sait lire `PARAM.SFO` résout celui d'un jeu PSP. Ces noms
 * doivent redescendre, sinon la bibliothèque affiche `000f3000` là où le
 * serveur sait écrire « Professor Layton and the Azran Legacy ».
 *
 * Deux refus, et un seul protège vraiment :
 *
 * 1. un libellé saisi **à la main** n'est jamais écrasé ;
 * 2. un titre déjà résolu n'est pas remplacé par un identifiant brut — un
 *    client ancien envoie le serial, et le laisser gagner ferait régresser la
 *    bibliothèque à chaque passe.
 *
 * Rend le libellé à écrire, ou `null` s'il ne faut rien changer. Même règle et
 * mêmes vecteurs que `server/app/core/labels.py` et que le noyau C++
 * (`tests/vectors/label_adoption.json`).
 */
fun adoptedLabel(
    storedLabel: String,
    storedSource: String,
    incomingLabel: String,
    unitKey: String,
): String? =
    when {
        storedSource != "auto" -> null
        incomingLabel.isEmpty() || incomingLabel == storedLabel -> null
        !isRawLabel(storedLabel, unitKey) && isRawLabel(incomingLabel, unitKey) -> null
        else -> incomingLabel
    }
