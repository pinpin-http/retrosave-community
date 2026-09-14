package com.retrosave.core.adapters.azahar

import com.retrosave.core.adapters.isValidRoot

/**
 * Détecte les dossiers de sauvegarde Azahar à partir d'une racine déjà choisie.
 *
 * Le parcours est strictement guidé par le contrat Azahar :
 *
 * `sdmc/Nintendo 3DS/<id0>/<id1>/title/00040000/<titleIdLow>/data`
 *
 * Il ne descend jamais dans un dossier de ROMs et n'ouvre aucun fichier. Chaque
 * niveau est listé une fois via [SafTreeReader], puis filtré avant de poursuivre.
 */
class AzaharSaveDetector {
    /**
     * Lance une détection synchrone. L'appelant Android l'exécute sur Dispatchers.IO.
     *
     * @param rootDocumentId identifiant SAF opaque de la racine Azahar.
     * @param reader abstraction de lecture des enfants, réelle ou simulée.
     */
    fun detect(
        rootDocumentId: String,
        reader: SafTreeReader,
        activeIdentity: String? = null,
    ): AzaharDetectionResult {
        val rootChildren = reader.children(rootDocumentId)
        val sdmc =
            rootChildren.directoryNamed(PRIMARY_MARKER)
                ?: return if (rootChildren.hasValidMarkers()) {
                    // Observation sur Azahar 2125.1.3 : une nouvelle racine contient
                    // déjà config/ et d'autres dossiers propres à l'émulateur, mais
                    // sdmc/ n'est créé qu'au premier lancement d'un jeu exploitable.
                    AzaharDetectionResult.Valid(emptyList())
                } else {
                    AzaharDetectionResult.InvalidRoot
                }

        // Nintendo 3DS/ peut ne pas exister avant toute initialisation d'Azahar.
        val nintendo3ds =
            reader.children(sdmc.documentId).directoryNamed("Nintendo 3DS")
                ?: return AzaharDetectionResult.Valid(emptyList())

        val identities = discoverIdentities(nintendo3ds, reader)
        if (identities.isEmpty()) {
            return AzaharDetectionResult.Valid(emptyList())
        }
        val selection =
            selectAzaharIdentity(
                identities = identities.map(IdentityCandidate::option),
                activeIdentity = activeIdentity,
            )
        if (selection.status == "choice_required") {
            return AzaharDetectionResult.IdentityChoiceRequired(
                identities = identities.map(IdentityCandidate::option),
                proposedIdentity = selection.proposedIdentity,
            )
        }
        val selected =
            identities.single { candidate ->
                candidate.option.identity == selection.activeIdentity
            }
        val units = buildList { collectUnitsForIdentity(selected, reader) }

        return AzaharDetectionResult.Valid(
            units =
                units
                    // Un provider peut exposer deux fois le même document ; ce doublon
                    // technique est retiré. En revanche, deux profils différents ayant
                    // le même title ID sont tous deux conservés pour ne rien masquer.
                    .distinctBy(AzaharSaveUnit::dataDocumentId)
                    .sortedWith(
                        compareBy(
                            AzaharSaveUnit::unitKey,
                            AzaharSaveUnit::dataDocumentId,
                        ),
                    ),
            activeIdentity = selected.option.identity,
            ignoredIdentities = selection.ignoredIdentities,
        )
    }

    /**
     * Découvre uniquement les couples portant un dossier `title/`.
     *
     * Avec plusieurs couples, aucun titre n'est parcouru avant le choix explicite
     * de l'utilisateur : une collision de `unit_key` ne peut donc jamais être
     * synchronisée silencieusement.
     */
    private fun discoverIdentities(
        nintendo3ds: SafTreeEntry,
        reader: SafTreeReader,
    ): List<IdentityCandidate> =
        buildList {
            reader
                .children(nintendo3ds.documentId)
                .validHexDirectories(ID_DIRECTORY_LENGTH)
                .forEach { id0 ->
                    reader
                        .children(id0.documentId)
                        .validHexDirectories(ID_DIRECTORY_LENGTH)
                        .forEach { id1 ->
                            val title =
                                reader.children(id1.documentId).directoryNamed("title")
                                    ?: return@forEach
                            val identity =
                                "${id0.displayName.lowercase()}/${id1.displayName.lowercase()}"
                            add(
                                IdentityCandidate(
                                    option =
                                        AzaharIdentityOption(
                                            identity = identity,
                                            latestModified =
                                                listOfNotNull(
                                                    id0.lastModified,
                                                    id1.lastModified,
                                                    title.lastModified,
                                                ).maxOrNull(),
                                        ),
                                    title = title,
                                ),
                            )
                        }
                }
        }.sortedBy { it.option.identity }

    /**
     * Cherche le sous-arbre des titres applicatifs pour un couple id0/id1.
     *
     * Les branches absentes sont normales : un profil peut n'avoir encore lancé
     * aucun jeu. Dans ce cas la fonction revient sans produire d'unité.
     */
    private fun MutableList<AzaharSaveUnit>.collectUnitsForIdentity(
        identity: IdentityCandidate,
        reader: SafTreeReader,
    ) {
        val applicationTitles =
            reader
                .children(identity.title.documentId)
                .directoryNamed(APPLICATION_TITLE_HIGH)
                ?: return

        reader
            .children(applicationTitles.documentId)
            .validHexDirectories(TITLE_ID_LOW_LENGTH)
            .forEach { titleIdLow ->
                // L'unité n'existe qu'une fois le dossier data/ réellement créé.
                val data = reader.children(titleIdLow.documentId).directoryNamed("data")
                if (data != null) {
                    add(
                        AzaharSaveUnit(
                            titleIdLow = titleIdLow.displayName.lowercase(),
                            dataDocumentId = data.documentId,
                        ),
                    )
                }
            }
    }

    /** Recherche un dossier sans rendre la détection sensible à la casse. */
    private fun List<SafTreeEntry>.directoryNamed(name: String): SafTreeEntry? =
        firstOrNull { entry ->
            entry.isDirectory && entry.displayName.equals(name, ignoreCase = true)
        }

    /** Écarte les fichiers et noms qui ne correspondent pas aux IDs 3DS. */
    private fun List<SafTreeEntry>.validHexDirectories(length: Int): List<SafTreeEntry> =
        filter { entry ->
            entry.isDirectory &&
                entry.displayName.length == length &&
                entry.displayName.all { character -> character.isHexDigit() }
        }.sortedBy { it.displayName.lowercase() }

    /**
     * Reconnaît une racine Azahar initialisée avant la création de `sdmc/`.
     *
     * `config/` seul serait un marqueur trop générique. Le détecteur exige donc
     * aussi au moins un second dossier qu'Azahar crée à côté. Cette heuristique
     * évite de traiter un dossier arbitraire comme une racine vide valide.
     */
    private fun List<SafTreeEntry>.hasValidMarkers(): Boolean =
        isValidRoot(
            presentMarkers =
                asSequence()
                    .filter(SafTreeEntry::isDirectory)
                    .map(SafTreeEntry::displayName)
                    .toList(),
            markersPrimary = setOf(PRIMARY_MARKER),
            markersSecondary = SECONDARY_MARKERS,
            markersSecondaryMin = SECONDARY_MARKERS_MIN,
        )

    /** Version locale pour ne pas accepter les caractères Unicode de isDigit(). */
    private fun Char.isHexDigit(): Boolean = this in '0'..'9' || this in 'a'..'f' || this in 'A'..'F'

    private companion object {
        const val ID_DIRECTORY_LENGTH = 32
        const val TITLE_ID_LOW_LENGTH = 8
        const val APPLICATION_TITLE_HIGH = "00040000"

        const val PRIMARY_MARKER = "sdmc"
        const val SECONDARY_MARKERS_MIN = 2
        val SECONDARY_MARKERS =
            setOf(
                "config",
                "gpu_drivers",
                "log",
                "nand",
                "sysdata",
            )
    }

    private data class IdentityCandidate(
        val option: AzaharIdentityOption,
        val title: SafTreeEntry,
    )
}
