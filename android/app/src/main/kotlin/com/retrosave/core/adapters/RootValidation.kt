package com.retrosave.core.adapters

/**
 * Applique la règle commune de validation des marqueurs d'une racine.
 *
 * Une racine est valide si elle contient au moins un marqueur primaire, ou le
 * nombre minimal demandé de marqueurs secondaires. Les noms sont comparés sans
 * tenir compte de la casse, comme dans l'implémentation Python.
 */
fun isValidRoot(
    presentMarkers: Collection<String>,
    markersPrimary: Collection<String>,
    markersSecondary: Collection<String>,
    markersSecondaryMin: Int,
): Boolean {
    val present = presentMarkers.map(String::lowercase).toSet()
    val primary = markersPrimary.map(String::lowercase).toSet()
    val secondary = markersSecondary.map(String::lowercase).toSet()

    return present.any(primary::contains) ||
        present.count(secondary::contains) >= markersSecondaryMin
}
