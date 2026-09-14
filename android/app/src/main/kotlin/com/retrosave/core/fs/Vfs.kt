package com.retrosave.core.fs

import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import java.io.InputStream

/** Port de fichiers pur utilisé à l'identique par le moteur et les tests JVM. */
interface Vfs {
    suspend fun listFiles(
        root: RootRef,
        subPath: String = "",
    ): List<NodeMeta>

    suspend fun readBytes(
        root: RootRef,
        relPath: String,
    ): ByteArray

    /** Ouvre un fichier en lecture ; l'appelant referme (AD-29). */
    suspend fun openRead(
        root: RootRef,
        relPath: String,
    ): InputStream

    suspend fun sha256(
        root: RootRef,
        relPath: String,
    ): ByteArray

    /**
     * Écrit depuis un flux et prouve le résultat avant de remplacer (Q13).
     *
     * Rien de plus gros que le tampon n'existe à la fois. Ne prend **aucun**
     * backup (Q27) : c'est une politique de niveau unité, qui appartient au
     * moteur.
     */
    suspend fun writeAtomic(
        root: RootRef,
        relPath: String,
        source: InputStream,
        expectedSha256: String,
    )

    suspend fun delete(
        root: RootRef,
        relPath: String,
    )

    suspend fun backup(
        root: RootRef,
        relPath: String,
    )

    suspend fun exists(
        root: RootRef,
        relPath: String,
    ): Boolean
}

class UnsupportedNodeException(
    message: String,
) : IllegalStateException(message)

class RootInaccessibleException(
    val rootId: String,
    cause: Throwable? = null,
) : IllegalStateException("Racine inaccessible : $rootId", cause)
