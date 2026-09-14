package com.retrosave.core.model

/** Métadonnées d'un fichier utilisées par l'empreinte rapide locale. */
data class NodeMeta(
    val relPath: String,
    val sizeBytes: Long,
    val mtimeMs: Long?,
)
