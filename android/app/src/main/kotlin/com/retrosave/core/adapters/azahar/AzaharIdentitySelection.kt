package com.retrosave.core.adapters.azahar

/** Résultat pur de la sélection AD-23 d'une identité locale active. */
data class AzaharIdentitySelection(
    val status: String,
    val activeIdentity: String?,
    val proposedIdentity: String?,
    val ignoredIdentities: List<String>,
)

/**
 * Sélectionne automatiquement une identité unique ou exige un choix explicite.
 *
 * Une identité configurée n'est appliquée que si elle fait partie des couples
 * réellement découverts. Avec plusieurs couples et aucun choix, le mtime le
 * plus récent est seulement proposé lorsqu'il désigne un candidat unique.
 */
fun selectAzaharIdentity(
    identities: List<AzaharIdentityOption>,
    activeIdentity: String?,
): AzaharIdentitySelection {
    val ordered = identities.sortedBy(AzaharIdentityOption::identity)
    val selected =
        ordered.firstOrNull {
            it.identity.equals(activeIdentity, ignoreCase = true)
        } ?: ordered.singleOrNull()
    if (selected != null) {
        return AzaharIdentitySelection(
            status = "selected",
            activeIdentity = selected.identity,
            proposedIdentity = null,
            ignoredIdentities =
                ordered
                    .map(AzaharIdentityOption::identity)
                    .filterNot(selected.identity::equals),
        )
    }

    val latest = ordered.mapNotNull(AzaharIdentityOption::latestModified).maxOrNull()
    val latestCandidates =
        ordered.filter {
            latest != null && it.latestModified == latest
        }
    return AzaharIdentitySelection(
        status = "choice_required",
        activeIdentity = null,
        proposedIdentity = latestCandidates.singleOrNull()?.identity,
        ignoredIdentities = emptyList(),
    )
}
