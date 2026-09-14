package com.retrosave.core.adapters.azahar

/**
 * Métadonnées minimales d'un enfant SAF.
 *
 * La détection ne demande jamais de flux de fichier : elle travaille seulement
 * avec l'identifiant opaque du document, son nom et son type MIME. Les taille et
 * date sont déjà projetées pour le futur calcul d'empreinte rapide.
 */
data class SafTreeEntry(
    val documentId: String,
    val displayName: String,
    val mimeType: String,
    val size: Long?,
    val lastModified: Long?,
) {
    /** Indique un dossier sans dépendre de classes Android dans les tests JVM. */
    val isDirectory: Boolean
        get() = mimeType == DIRECTORY_MIME_TYPE

    companion object {
        // Valeur standard de DocumentsContract.Document.MIME_TYPE_DIR.
        // Elle est recopiée ici pour que ce modèle reste un pur type Kotlin.
        const val DIRECTORY_MIME_TYPE = "vnd.android.document/directory"
    }
}

/**
 * Une unité de sauvegarde Azahar détectée.
 *
 * [dataDocumentId] est l'identifiant SAF du dossier `data/`, pas un chemin
 * physique. Le provider Android reste donc maître de l'accès réel.
 */
data class AzaharSaveUnit(
    val titleIdLow: String,
    val dataDocumentId: String,
) {
    /** Clé normative du brief pour retrouver l'unité côté serveur. */
    val unitKey: String = titleIdLow

    /** Clé de regroupement multi-appareils, indépendante du chemin local. */
    val gameKey: String = "3ds:$titleIdLow"

    /** Libellé POC ; l'utilisateur pourra le renommer plus tard. */
    val gameLabel: String = titleIdLow
}

/** Une identité 3DS locale candidate, indépendante de la clé serveur du jeu. */
data class AzaharIdentityOption(
    val identity: String,
    val latestModified: Long?,
)

/**
 * Résultat du cœur de détection pur.
 *
 * Une racine est également valide si elle porte les marqueurs d'une
 * installation Azahar fraîche, même lorsque `sdmc/` n'existe pas encore.
 * Dans les deux cas, [Valid.units] peut être vide avant la première sauvegarde.
 */
sealed interface AzaharDetectionResult {
    data object InvalidRoot : AzaharDetectionResult

    data class IdentityChoiceRequired(
        val identities: List<AzaharIdentityOption>,
        val proposedIdentity: String?,
    ) : AzaharDetectionResult

    data class Valid(
        val units: List<AzaharSaveUnit>,
        val activeIdentity: String? = null,
        val ignoredIdentities: List<String> = emptyList(),
    ) : AzaharDetectionResult
}

/**
 * États présentables par l'interface Android.
 *
 * Ils distinguent volontairement une racine invalide, un accès révoqué et une
 * racine valide mais vide afin de ne jamais transformer une absence en erreur
 * ou, plus tard, en suppression distante.
 */
sealed interface AzaharScanResult {
    data object NoRoot : AzaharScanResult

    data object Scanning : AzaharScanResult

    data object InvalidRoot : AzaharScanResult

    data object AccessLost : AzaharScanResult

    data class Empty(
        val activeIdentity: String?,
        val ignoredIdentities: List<String>,
    ) : AzaharScanResult

    data class IdentityChoiceRequired(
        val identities: List<AzaharIdentityOption>,
        val proposedIdentity: String?,
    ) : AzaharScanResult

    data class Found(
        val units: List<AzaharSaveUnit>,
        val activeIdentity: String?,
        val ignoredIdentities: List<String>,
    ) : AzaharScanResult

    data class Failure(
        val message: String,
    ) : AzaharScanResult
}
