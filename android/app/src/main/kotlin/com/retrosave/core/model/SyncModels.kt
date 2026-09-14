package com.retrosave.core.model

/** Référence opaque d'une racine ; seul le VFS sait la résoudre. */
data class RootRef(
    val rootId: String,
)

/** Une unité locale découverte par un adaptateur. */
data class UnitRef(
    val emulator: String,
    val unitKey: String,
    val unitType: String,
    val gameKey: String,
    val gameLabel: String,
    val root: RootRef,
    val relPath: String,
)

/** Une unité et les métadonnées de ses fichiers, sans contenu ouvert. */
data class DiscoveredUnit(
    val unit: UnitRef,
    val nodes: List<NodeMeta>,
    val unsupportedReason: String? = null,
    val duplicateRelPaths: List<String> = emptyList(),
)

/** Miroir local exact de l'état §6.1. */
data class LocalUnit(
    val localId: Long,
    val serverId: String?,
    val emulator: String,
    val unitKey: String,
    val unitType: String,
    val gameKey: String,
    val gameLabel: String,
    val rootId: String,
    val relPath: String,
    val observedQfHash: String?,
    val observedAt: Long?,
    val stableQfHash: String?,
    val stableSince: Long?,
    val lastSyncedVersion: Int,
    val lastSyncedContentSha256: String?,
    val state: String,
    val openConflictId: String?,
    val pendingBranchNumber: Int?,
    val pendingBranchContentSha256: String?,
    // AD-27 : posé avant la première écriture d'une application, effacé après
    // la mise à jour de lastSynced. Présent au démarrage d'une passe = une
    // application a été interrompue.
    val applyJournalVersion: Int? = null,
    val applyJournalContentSha256: String? = null,
    val applyJournalStartedAt: Long? = null,
    // AD-30 : casse-boucle. Une unité qui fait tomber le process trois fois de
    // suite sur la même tête est mise de côté — ce qu'aucun `catch` ne peut
    // garantir, un crash natif ou un kill système n'exécutant aucun bloc.
    val consecutiveFailures: Int = 0,
    val lastFailureHead: Int? = null,
    // M8 §7 : 'user' verrouille le libellé contre toute résolution automatique.
    val labelSource: String = "auto",
)

/** Compteurs stables présentés après une passe. */
data class SyncReport(
    var pushed: Int = 0,
    var pulled: Int = 0,
    var conflicts: Int = 0,
    var errors: Int = 0,
    var paused: Boolean = false,
    var retryable: Boolean = false,
    val messages: MutableList<String> = mutableListOf(),
    // Q20 : les messages d'exception vont au journal, jamais à l'UI. Le moteur
    // rend l'état typé — quels adaptateurs ont perdu l'accès — et l'écran
    // construit sa phrase. Un identifiant interne n'a rien à faire à l'écran.
    val inaccessibleAdapters: MutableSet<String> = mutableSetOf(),
    // Q29 : « à placer » au sens d'AD-26 — cible incalculable, on refuse de
    // deviner un chemin. Structurellement réservé à melonDS, seul adaptateur
    // sans cible canonique (0 ou ≥ 2 correspondances de nom). La condition
    // elle-même est un vecteur partagé, `melonds_placement_basic.json`, donc
    // identique dans les deux moteurs par construction.
    val awaitingPlacement: MutableList<String> = mutableListOf(),
    // §9.6 : « non configuré » n'est pas « à placer ». L'adaptateur n'a aucune
    // racine sur cet appareil ; dire à l'utilisateur de lancer un jeu
    // l'enverrait faire la mauvaise chose. Trouvé sur le Thor le 02/08, où deux
    // unités `folder` du PC s'affichaient comme « à placer ».
    val unconfiguredAdapters: MutableSet<String> = mutableSetOf(),
)
