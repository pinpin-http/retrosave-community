package com.retrosave.core.fs

const val RENAME_ACCEPTED = "accepted"
const val RENAME_DEVIATED = "deviated"
const val RENAME_REFUSED = "refused"

/**
 * Classe un rename par le nom réellement obtenu, jamais par son code retour (AD-27).
 *
 * Un provider peut « réussir » en créant `nom (1).ext` : c'est une déviation,
 * pas un succès, et l'appelant ne doit pas la prendre pour une cible écrite.
 */
fun renameOutcome(
    requested: String,
    obtained: String?,
): String =
    when (obtained) {
        null -> RENAME_REFUSED
        requested -> RENAME_ACCEPTED
        else -> RENAME_DEVIATED
    }

const val PATH_OK = "ok"
const val PATH_REJECT = "reject"

/**
 * Étage lexical anti-zip-slip d'AD-28 : refuse absolus et échappements parents.
 *
 * S'applique aux chemins venus de l'extérieur (entrées d'archive, cibles
 * distantes), jamais à ceux que le scan a construits sous la racine.
 */
fun pathGuard(relPath: String): String {
    if (relPath.isEmpty()) return PATH_REJECT
    val normalized = relPath.replace('\\', '/')
    if (normalized.startsWith("/")) return PATH_REJECT
    val head = normalized.substringBefore('/')
    if (head.length >= 2 && head[1] == ':') return PATH_REJECT
    if (normalized.split('/').any { it == ".." }) return PATH_REJECT
    return PATH_OK
}
