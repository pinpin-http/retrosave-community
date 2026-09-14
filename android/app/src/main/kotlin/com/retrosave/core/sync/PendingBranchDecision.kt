package com.retrosave.core.sync

/**
 * Applique AD-24/Q5-bis avant le reste de la table de décision push/pull.
 *
 * `null` signifie que ce fragment ne tranche pas et que le moteur poursuit les
 * autres règles. Un conflit ouvert bloque toujours ; une branche locale déjà
 * représentée côté serveur force un pull après résolution.
 */
fun pendingBranchDecision(
    openConflict: Boolean,
    localContentSha256: String,
    pendingBranchContentSha256: String?,
): String? {
    if (openConflict) {
        return "blocked_conflict"
    }
    if (
        pendingBranchContentSha256 != null &&
        localContentSha256 == pendingBranchContentSha256
    ) {
        return "pull"
    }
    return null
}
