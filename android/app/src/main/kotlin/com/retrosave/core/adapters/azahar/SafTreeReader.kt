package com.retrosave.core.adapters.azahar

/**
 * Port minimal utilisé par le détecteur pour lister les enfants d'un dossier.
 *
 * L'interface découple la règle métier d'Android : les tests fournissent une
 * arborescence en mémoire, tandis que l'application utilise ContentResolver.
 * Elle n'expose volontairement aucune méthode pour ouvrir le contenu d'un
 * fichier, ce qui rend impossible toute lecture de ROM dans ce POC.
 */
fun interface SafTreeReader {
    fun children(documentId: String): List<SafTreeEntry>
}
