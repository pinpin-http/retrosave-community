package com.retrosave.core.adapters.azahar

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests JVM du cœur de détection.
 *
 * Ils n'utilisent aucune classe Android ni aucun appareil : [FakeTreeReader]
 * remplace SAF par une arborescence en mémoire et enregistre chaque dossier
 * consulté. Cela permet aussi de prouver que `roms/` n'est jamais parcouru.
 */
class AzaharSaveDetectorTest {
    private val detector = AzaharSaveDetector()

    @Test
    fun `rejects an arbitrary root without sdmc or azahar markers`() {
        // Given : l'utilisateur a choisi par erreur son dossier de ROMs.
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("roms", "roms")),
            )

        // When : la racine est validée.
        val result = detector.detect("root", reader)

        // Then : elle est rejetée et son contenu n'est jamais inspecté.
        assertEquals(AzaharDetectionResult.InvalidRoot, result)
        assertFalse(reader.queriedDocumentIds.contains("roms"))
    }

    @Test
    fun `accepts a freshly initialized azahar root before sdmc exists`() {
        // Given : état réellement observé sur Azahar 2125.1.3 avant qu'un jeu
        // exploitable n'ait été lancé. Aucun de ces dossiers n'est parcouru.
        val reader =
            FakeTreeReader(
                "root" to
                    listOf(
                        directory("config", "config"),
                        directory("gpu", "gpu_drivers"),
                        directory("log", "log"),
                        directory("nand", "nand"),
                    ),
            )

        val result = detector.detect("root", reader)

        assertEquals(AzaharDetectionResult.Valid(emptyList()), result)
        assertEquals(listOf("root"), reader.queriedDocumentIds)
    }

    @Test
    fun `rejects config alone as an insufficient azahar marker`() {
        // Un dossier de projet nommé config/ ne doit pas devenir une fausse
        // racine Azahar vide sans au moins un second marqueur spécifique.
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("config", "config")),
            )

        assertEquals(AzaharDetectionResult.InvalidRoot, detector.detect("root", reader))
    }

    @Test
    fun `accepts a valid sdmc before any game has created its 3ds tree`() {
        // Given : sdmc/ existe déjà, mais aucun jeu n'a encore créé Nintendo 3DS/.
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("sdmc", "sdmc")),
                "sdmc" to emptyList(),
            )

        // When / Then : la racine est valide et la liste est simplement vide.
        val result = detector.detect("root", reader)

        assertEquals(AzaharDetectionResult.Valid(emptyList()), result)
    }

    @Test
    fun `detects data directories only for the explicitly active identity`() {
        val firstId0 = "a".repeat(32)
        val firstId1 = "1".repeat(32)
        val secondId0 = "b".repeat(32)
        val secondId1 = "2".repeat(32)

        // Given : deux profils valides, plus plusieurs branches qui doivent être ignorées.
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("sdmc", "sdmc"), directory("roms", "roms")),
                "sdmc" to listOf(directory("n3ds", "Nintendo 3DS")),
                "n3ds" to
                    listOf(
                        directory("id0-a", firstId0),
                        directory("id0-b", secondId0),
                        directory("invalid-id0", "not-a-console-id"),
                    ),
                "id0-a" to listOf(directory("id1-a", firstId1)),
                "id0-b" to listOf(directory("id1-b", secondId1)),
                "id1-a" to listOf(directory("title-a", "title")),
                "id1-b" to listOf(directory("title-b", "title")),
                "title-a" to listOf(directory("apps-a", "00040000")),
                "title-b" to listOf(directory("apps-b", "00040000")),
                "apps-a" to
                    listOf(
                        directory("game-a", "00ABCDEF"),
                        directory("ignored-system-title", "00040010"),
                        directory("bad-title", "rom.3ds"),
                    ),
                "apps-b" to listOf(directory("game-b", "00112233")),
                "game-a" to listOf(directory("data-a", "data")),
                "ignored-system-title" to emptyList(),
                "game-b" to listOf(directory("data-b", "DATA")),
            )

        // When : la première identité a été choisie explicitement.
        val result =
            detector.detect(
                "root",
                reader,
                activeIdentity = "$firstId0/$firstId1",
            ) as AzaharDetectionResult.Valid

        // Then : seule l'identité active est parcourue jusqu'aux sauvegardes.
        assertEquals(
            listOf("00abcdef"),
            result.units.map(AzaharSaveUnit::unitKey),
        )
        assertEquals("3ds:00abcdef", result.units.single().gameKey)
        assertEquals("$firstId0/$firstId1", result.activeIdentity)
        assertEquals(listOf("$secondId0/$secondId1"), result.ignoredIdentities)
        assertFalse(reader.queriedDocumentIds.contains("roms"))
        assertFalse(reader.queriedDocumentIds.contains("invalid-id0"))
        assertFalse(reader.queriedDocumentIds.contains("bad-title"))
        assertFalse(reader.queriedDocumentIds.contains("title-b"))
    }

    @Test
    fun `does not report a title until its data directory exists`() {
        val id0 = "a".repeat(32)
        val id1 = "b".repeat(32)
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("sdmc", "sdmc")),
                "sdmc" to listOf(directory("n3ds", "Nintendo 3DS")),
                "n3ds" to listOf(directory("id0", id0)),
                "id0" to listOf(directory("id1", id1)),
                "id1" to listOf(directory("title", "title")),
                "title" to listOf(directory("apps", "00040000")),
                "apps" to listOf(directory("game", "00112233")),
                "game" to listOf(directory("content", "content")),
            )

        // Le title ID seul ne suffit pas : le dossier data/ matérialise la sauvegarde.
        val result = detector.detect("root", reader) as AzaharDetectionResult.Valid

        assertTrue(result.units.isEmpty())
    }

    @Test
    fun `requires a choice when two identities contain the same title id`() {
        val firstId0 = "a".repeat(32)
        val firstId1 = "1".repeat(32)
        val secondId0 = "b".repeat(32)
        val secondId1 = "2".repeat(32)

        // Given : deux identités 3DS possèdent chacune une sauvegarde du même jeu.
        val reader =
            FakeTreeReader(
                "root" to listOf(directory("sdmc", "sdmc")),
                "sdmc" to listOf(directory("n3ds", "Nintendo 3DS")),
                "n3ds" to
                    listOf(
                        directory("id0-a", firstId0),
                        directory("id0-b", secondId0),
                    ),
                "id0-a" to listOf(directory("id1-a", firstId1, lastModified = 100)),
                "id0-b" to listOf(directory("id1-b", secondId1, lastModified = 200)),
                "id1-a" to listOf(directory("title-a", "title")),
                "id1-b" to listOf(directory("title-b", "title")),
                "title-a" to listOf(directory("apps-a", "00040000")),
                "title-b" to listOf(directory("apps-b", "00040000")),
                "apps-a" to listOf(directory("game-a", "00112233")),
                "apps-b" to listOf(directory("game-b", "00112233")),
                "game-a" to listOf(directory("data-a", "data")),
                "game-b" to listOf(directory("data-b", "data")),
            )

        val choice =
            detector.detect("root", reader) as
                AzaharDetectionResult.IdentityChoiceRequired

        // Aucun contenu d'identité n'est inspecté tant que le choix n'est pas fait.
        assertEquals(
            listOf("$firstId0/$firstId1", "$secondId0/$secondId1"),
            choice.identities.map(AzaharIdentityOption::identity),
        )
        assertEquals("$secondId0/$secondId1", choice.proposedIdentity)
        assertFalse(reader.queriedDocumentIds.contains("title-a"))
        assertFalse(reader.queriedDocumentIds.contains("title-b"))

        val selected =
            detector.detect(
                "root",
                reader,
                activeIdentity = "$firstId0/$firstId1",
            ) as AzaharDetectionResult.Valid

        assertEquals(listOf("00112233"), selected.units.map(AzaharSaveUnit::unitKey))
        assertEquals(listOf("$secondId0/$secondId1"), selected.ignoredIdentities)
        assertFalse(reader.queriedDocumentIds.contains("title-b"))
    }

    /** Construit un dossier SAF fictif sans dépendre de DocumentsContract. */
    private fun directory(
        documentId: String,
        name: String,
        lastModified: Long? = null,
    ) = SafTreeEntry(
        documentId = documentId,
        displayName = name,
        mimeType = SafTreeEntry.DIRECTORY_MIME_TYPE,
        size = null,
        lastModified = lastModified,
    )

    /**
     * Lecteur déterministe utilisé par les tests.
     *
     * Une clé absente représente un dossier vide. [queriedDocumentIds] permet
     * d'asserter les branches que le détecteur a réellement consultées.
     */
    private class FakeTreeReader(
        vararg entries: Pair<String, List<SafTreeEntry>>,
    ) : SafTreeReader {
        private val childrenByDocumentId = mapOf(*entries)
        val queriedDocumentIds = mutableListOf<String>()

        override fun children(documentId: String): List<SafTreeEntry> {
            // Trace la requête avant de rendre les enfants simulés.
            queriedDocumentIds += documentId
            return childrenByDocumentId[documentId].orEmpty()
        }
    }
}
