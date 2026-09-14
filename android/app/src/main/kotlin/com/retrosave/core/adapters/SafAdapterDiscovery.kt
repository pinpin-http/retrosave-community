package com.retrosave.core.adapters

import com.retrosave.core.adapters.azahar.SafTreeEntry
import com.retrosave.core.adapters.azahar.SafTreeReader
import java.util.ArrayDeque

data class SafDirectoryCandidate(
    val name: String,
    val relPath: String,
)

data class SafFileCandidate(
    val name: String,
    val relPath: String,
    val sizeBytes: Long,
    val mtimeMs: Long?,
)

data class PpssppRootCandidates(
    val effectivePrefix: String,
    val directories: List<SafDirectoryCandidate>,
)

fun discoverPpssppCandidates(
    rootDocumentId: String,
    reader: SafTreeReader,
): PpssppRootCandidates {
    val rootChildren = reader.children(rootDocumentId)
    val savedata =
        rootChildren.firstOrNull {
            it.isDirectory && it.displayName.equals("SAVEDATA", ignoreCase = true)
        }
    val containsPspSiblings =
        savedata != null &&
            rootChildren.any {
                it.isDirectory &&
                    it.displayName.equals("GAME", ignoreCase = true)
            }
    val effectiveDocumentId =
        if (containsPspSiblings) {
            checkNotNull(savedata).documentId
        } else {
            rootDocumentId
        }
    val effectivePrefix = if (containsPspSiblings) checkNotNull(savedata).displayName else ""
    val directories =
        reader
            .children(effectiveDocumentId)
            .asSequence()
            .filter(SafTreeEntry::isDirectory)
            .filterNot { it.displayName.lowercase() in RESERVED_PSP_DIRECTORIES }
            .filterNot { isRetroSaveInternal(it.displayName) }
            .map {
                SafDirectoryCandidate(
                    name = it.displayName,
                    relPath = join(effectivePrefix, it.displayName),
                )
            }.sortedBy(SafDirectoryCandidate::name)
            .toList()
    return PpssppRootCandidates(effectivePrefix, directories)
}

data class MelonDsRootCandidates(
    val saves: List<SafFileCandidate>,
    /** Noms de ROM relevés pour le placement AD-26 ; jamais ouverts ni lus (I1). */
    val romRelPaths: List<String>,
)

fun discoverMelonDsCandidates(
    rootDocumentId: String,
    reader: SafTreeReader,
    maxDepth: Int,
): MelonDsRootCandidates {
    require(maxDepth >= 1)
    val saves = mutableListOf<SafFileCandidate>()
    val romRelPaths = mutableListOf<String>()
    val queue = ArrayDeque<QueuedSafDirectory>()
    queue.add(QueuedSafDirectory(rootDocumentId, "", 0))
    while (queue.isNotEmpty()) {
        val current = queue.removeFirst()
        reader.children(current.documentId).forEach { child ->
            if (isRetroSaveInternal(child.displayName)) return@forEach
            val relPath = join(current.relPath, child.displayName)
            val depth = current.depth + 1
            when {
                child.isDirectory && depth < maxDepth ->
                    queue.add(
                        QueuedSafDirectory(
                            documentId = child.documentId,
                            relPath = relPath,
                            depth = depth,
                        ),
                    )
                child.isDirectory -> Unit
                depth > maxDepth -> Unit
                child.displayName.endsWith(".sav", ignoreCase = true) ->
                    saves +=
                        SafFileCandidate(
                            name = child.displayName,
                            relPath = relPath,
                            sizeBytes = child.size ?: 0,
                            mtimeMs = child.lastModified,
                        )
                child.displayName.endsWith(".nds", ignoreCase = true) -> romRelPaths += relPath
            }
        }
    }
    return MelonDsRootCandidates(
        saves = saves.sortedBy(SafFileCandidate::relPath),
        romRelPaths = romRelPaths.sorted(),
    )
}

fun isRetroSaveInternal(name: String): Boolean =
    name.startsWith(".rsc-") ||
        name.endsWith(".rsc-tmp") ||
        name.endsWith(".rsc-bak") ||
        name.endsWith(".rsc-bak.1") ||
        name.endsWith(".rsc-bak.2")

private fun join(
    parent: String,
    child: String,
): String = if (parent.isEmpty()) child else "$parent/$child"

private data class QueuedSafDirectory(
    val documentId: String,
    val relPath: String,
    val depth: Int,
)

private val RESERVED_PSP_DIRECTORIES = setOf("game", "system", "ppsspp_state")
