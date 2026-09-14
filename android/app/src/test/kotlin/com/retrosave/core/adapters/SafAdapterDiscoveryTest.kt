package com.retrosave.core.adapters

import com.retrosave.core.adapters.azahar.SafTreeEntry
import com.retrosave.core.adapters.azahar.SafTreeReader
import com.retrosave.core.sync.quarantineState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class SafAdapterDiscoveryTest {
    @Test
    fun `PPSSPP parent root is redirected only into SAVEDATA`() {
        val reader =
            Tree(
                mapOf(
                    "root" to
                        listOf(
                            directory("save-root", "SAVEDATA"),
                            directory("game-root", "GAME"),
                            directory("system-root", "SYSTEM"),
                        ),
                    "save-root" to
                        listOf(
                            directory("unit", "ULES01234DATA"),
                            directory("state", "PPSSPP_STATE"),
                        ),
                ),
            )

        val result = discoverPpssppCandidates("root", reader)

        assertEquals("SAVEDATA", result.effectivePrefix)
        assertEquals(
            listOf("SAVEDATA/ULES01234DATA"),
            result.directories.map(SafDirectoryCandidate::relPath),
        )
        assertFalse(reader.visited.contains("game-root"))
        assertFalse(reader.visited.contains("system-root"))
    }

    @Test
    fun `melonDS only returns sav metadata within bounded depth`() {
        val reader =
            Tree(
                mapOf(
                    "root" to
                        listOf(
                            file("rom", "Game.nds", 1_000),
                            file("save", "Game.sav", 12),
                            directory("nested", "nested"),
                        ),
                    "nested" to
                        listOf(
                            file("nested-rom", "Other.nds", 2_000),
                            file("nested-save", "Other.SAV", 13),
                            directory("too-deep", "deep"),
                        ),
                    "too-deep" to listOf(file("ignored-save", "Ignored.sav", 14)),
                ),
            )

        val result = discoverMelonDsCandidates("root", reader, maxDepth = 2)

        assertEquals(
            listOf("Game.sav", "nested/Other.SAV"),
            result.saves.map(SafFileCandidate::relPath),
        )
        assertFalse(result.saves.any { it.name.endsWith(".nds") })
        assertEquals(listOf("Game.nds", "nested/Other.nds"), result.romRelPaths)
        assertFalse(reader.visited.contains("too-deep"))
    }

    @Test
    fun `melonDS reports colliding save names without dropping either path`() {
        val reader =
            Tree(
                mapOf(
                    "root" to listOf(directory("a", "a"), directory("b", "b")),
                    "a" to listOf(file("a-save", "same.sav", 10)),
                    "b" to listOf(file("b-save", "same.sav", 11)),
                ),
            )

        val result = discoverMelonDsCandidates("root", reader, maxDepth = 2)

        assertEquals(
            listOf("a/same.sav", "b/same.sav"),
            result.saves.map(SafFileCandidate::relPath),
        )
        assertEquals("duplicate", quarantineState(result.saves.map { it.relPath }))
    }

    private class Tree(
        private val nodes: Map<String, List<SafTreeEntry>>,
    ) : SafTreeReader {
        val visited = mutableSetOf<String>()

        override fun children(documentId: String): List<SafTreeEntry> {
            visited += documentId
            return nodes[documentId].orEmpty()
        }
    }

    private companion object {
        fun directory(
            id: String,
            name: String,
        ) = SafTreeEntry(
            documentId = id,
            displayName = name,
            mimeType = SafTreeEntry.DIRECTORY_MIME_TYPE,
            size = null,
            lastModified = 1,
        )

        fun file(
            id: String,
            name: String,
            size: Long,
        ) = SafTreeEntry(
            documentId = id,
            displayName = name,
            mimeType = "application/octet-stream",
            size = size,
            lastModified = 1,
        )
    }
}
