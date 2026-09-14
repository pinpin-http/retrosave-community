package com.retrosave.data.fs

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import com.retrosave.core.adapters.azahar.SafTreeEntry
import com.retrosave.core.adapters.azahar.SafTreeReader

/**
 * Fabrique un lecteur lié à une racine SAF précise.
 *
 * Une factory est injectée dans le dépôt afin de pouvoir remplacer entièrement
 * l'accès Android pendant les tests du domaine.
 */
fun interface SafTreeReaderFactory {
    fun create(treeUri: Uri): SafTreeReader
}

/** Implémentation de production de [SafTreeReaderFactory]. */
class AndroidSafTreeReaderFactory(
    private val contentResolver: ContentResolver,
) : SafTreeReaderFactory {
    override fun create(treeUri: Uri): SafTreeReader =
        AndroidSafTreeReader(
            contentResolver = contentResolver,
            treeUri = treeUri,
        )
}

/**
 * Adaptateur performant entre SAF et le modèle pur [SafTreeEntry].
 *
 * Il utilise directement [ContentResolver.query] sur l'URI des enfants. Cela
 * évite `DocumentFile.listFiles()` récursif, trop lent sur de grandes
 * arborescences et explicitement interdit par le brief.
 */
private class AndroidSafTreeReader(
    private val contentResolver: ContentResolver,
    private val treeUri: Uri,
) : SafTreeReader {
    /**
     * Liste un seul niveau de l'arbre, sans ouvrir le contenu des documents.
     *
     * [documentId] reste un identifiant opaque fourni par le DocumentsProvider ;
     * il n'est jamais converti en chemin de fichier local.
     */
    override fun children(documentId: String): List<SafTreeEntry> {
        val childrenUri =
            DocumentsContract.buildChildDocumentsUriUsingTree(
                treeUri,
                documentId,
            )

        return buildList {
            // Une requête unique récupère toutes les métadonnées nécessaires au
            // scan rapide futur : nom, type, taille et dernière modification.
            contentResolver
                .query(
                    childrenUri,
                    PROJECTION,
                    null,
                    null,
                    null,
                )?.use { cursor ->
                    val documentIdIndex =
                        cursor.getColumnIndexOrThrow(
                            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        )
                    val displayNameIndex =
                        cursor.getColumnIndexOrThrow(
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        )
                    val mimeTypeIndex =
                        cursor.getColumnIndexOrThrow(
                            DocumentsContract.Document.COLUMN_MIME_TYPE,
                        )
                    val sizeIndex =
                        cursor.getColumnIndexOrThrow(
                            DocumentsContract.Document.COLUMN_SIZE,
                        )
                    val modifiedIndex =
                        cursor.getColumnIndexOrThrow(
                            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
                        )

                    while (cursor.moveToNext()) {
                        // Un document sans nom ne peut pas correspondre à un segment
                        // Azahar connu ; il est donc ignoré sans tentative de lecture.
                        val displayName = cursor.getString(displayNameIndex) ?: continue
                        add(
                            SafTreeEntry(
                                documentId = cursor.getString(documentIdIndex),
                                displayName = displayName,
                                mimeType = cursor.getString(mimeTypeIndex),
                                size = cursor.nullableLong(sizeIndex),
                                lastModified = cursor.nullableLong(modifiedIndex),
                            ),
                        )
                    }
                }
        }
    }

    /** Convertit proprement les colonnes facultatives du provider. */
    private fun android.database.Cursor.nullableLong(columnIndex: Int): Long? = if (isNull(columnIndex)) null else getLong(columnIndex)

    private companion object {
        /**
         * Projection volontairement petite. Aucune colonne de contenu ni aucun
         * flux de fichier n'est demandé au provider.
         */
        val PROJECTION =
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED,
            )
    }
}
