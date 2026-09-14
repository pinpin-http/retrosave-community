package com.retrosave.data.adapters

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import com.retrosave.core.adapters.AdapterSpec
import com.retrosave.core.adapters.MAX_SFO_BYTES
import com.retrosave.core.adapters.azahar.AzaharDetectionResult
import com.retrosave.core.adapters.azahar.AzaharIdentityOption
import com.retrosave.core.adapters.azahar.AzaharSaveDetector
import com.retrosave.core.adapters.discoverMelonDsCandidates
import com.retrosave.core.adapters.discoverPpssppCandidates
import com.retrosave.core.adapters.displayName
import com.retrosave.core.adapters.melonDsRomDirectory
import com.retrosave.core.adapters.normalizeGameName
import com.retrosave.core.adapters.parseParamSfo
import com.retrosave.core.fs.RootInaccessibleException
import com.retrosave.core.fs.UnsupportedNodeException
import com.retrosave.core.fs.Vfs
import com.retrosave.core.model.DiscoveredUnit
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import com.retrosave.core.model.UnitRef
import com.retrosave.core.sync.RemoteTargetResolver
import com.retrosave.core.sync.RemoteUnit
import com.retrosave.core.sync.UnitDiscovery
import com.retrosave.core.sync.quarantineState
import com.retrosave.data.db.daos.RootDao
import com.retrosave.data.db.entities.RootEntity
import com.retrosave.data.fs.SafTreeReaderFactory
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

data class AdapterRootStatus(
    val rootId: String,
    val emulator: String,
    val accessible: Boolean,
    val unitCount: Int,
    val warning: String? = null,
    val activeIdentity: String? = null,
    val ignoredIdentities: List<String> = emptyList(),
)

class AzaharIdentityChoiceException(
    val rootId: String,
    val identities: List<AzaharIdentityOption>,
    val proposedIdentity: String?,
) : IllegalStateException(
        "Several Azahar identities are present; an explicit choice is required.",
    )

class AndroidAdapterRegistry(
    private val contentResolver: ContentResolver,
    private val roots: RootDao,
    private val vfs: Vfs,
    private val specs: Map<String, AdapterSpec>,
    private val readerFactory: SafTreeReaderFactory,
    private val azaharDetector: AzaharSaveDetector,
) : UnitDiscovery,
    RemoteTargetResolver {
    private val mutex = Mutex()
    private val _statuses = MutableStateFlow<Map<String, AdapterRootStatus>>(emptyMap())
    val statuses: StateFlow<Map<String, AdapterRootStatus>> = _statuses.asStateFlow()

    @Volatile
    private var targets = TargetSnapshot()

    override suspend fun discover(): List<DiscoveredUnit> =
        mutex.withLock {
            withContext(Dispatchers.IO) {
                val discovered = mutableListOf<DiscoveredUnit>()
                val rootTargets = mutableListOf<RootTarget>()
                val statuses = mutableMapOf<String, AdapterRootStatus>()
                roots
                    .listAll()
                    .filter { it.emulator in specs }
                    .forEach { root ->
                        try {
                            val result = discoverRoot(root)
                            discovered += result.units
                            rootTargets += result.target
                            statuses[root.rootId] = result.status
                        } catch (error: RootInaccessibleException) {
                            statuses[root.rootId] =
                                AdapterRootStatus(
                                    rootId = root.rootId,
                                    emulator = root.emulator,
                                    accessible = false,
                                    unitCount = 0,
                                    warning = "Autorisation SAF perdue.",
                                )
                            _statuses.value = statuses.toMap()
                            throw error
                        }
                    }

                // AD-26 : la collision met la clé en quarantaine, elle n'arrête pas le scan.
                val quarantined =
                    discovered
                        .groupBy { it.unit.emulator to it.unit.unitKey }
                        .mapNotNull { (identity, units) ->
                            val relPaths = units.map { it.unit.relPath }.distinct()
                            if (quarantineState(relPaths) == "duplicate") {
                                identity to units.first().copy(duplicateRelPaths = relPaths)
                            } else {
                                null
                            }
                        }.toMap()
                val resolved =
                    discovered.filterNot { (it.unit.emulator to it.unit.unitKey) in quarantined } +
                        quarantined.values
                targets =
                    TargetSnapshot(
                        existing =
                            resolved.associate {
                                (it.unit.emulator to it.unit.unitKey) to it.unit
                            },
                        rootsByEmulator = rootTargets.groupBy(RootTarget::emulator),
                    )
                _statuses.value = statuses.toMap()
                resolved
            }
        }

    override fun resolve(remote: RemoteUnit): UnitRef? {
        targets.existing[remote.emulator to remote.unitKey]?.let { return it }
        val emulatorRoots = targets.rootsByEmulator[remote.emulator].orEmpty()
        if (emulatorRoots.size != 1) return null
        val target = emulatorRoots.single()
        return when (remote.emulator) {
            "ppsspp" -> target.ppssppTarget(remote)
            "melonds" -> target.melonDsTarget(remote)
            "azahar" -> target.azaharTarget(remote)
            else -> null
        }
    }

    suspend fun configuredRoots(): Map<String, List<RootRef>> =
        roots
            .listAll()
            .filter { it.emulator in specs }
            .groupBy(
                keySelector = RootEntity::emulator,
                valueTransform = { RootRef(it.rootId) },
            )

    private suspend fun discoverRoot(root: RootEntity): RootDiscoveryResult {
        val uri = Uri.parse(root.treeUri)
        ensureReadable(root, uri)
        val rootDocumentId =
            try {
                DocumentsContract.getTreeDocumentId(uri)
            } catch (error: RuntimeException) {
                throw RootInaccessibleException(root.rootId, error)
            }
        check(!rootDocumentId.pointsInsideAndroidData()) {
            "Android/data is out of SAF scope."
        }
        val reader = readerFactory.create(uri)
        val rootRef = RootRef(root.rootId)
        return when (root.emulator) {
            "ppsspp" -> discoverPpsspp(root, rootRef, rootDocumentId, reader)
            "melonds" -> discoverMelonDs(root, rootRef, rootDocumentId, reader)
            "azahar" -> discoverAzahar(root, rootRef, rootDocumentId, reader)
            else -> error("Adaptateur Android inactif : ${root.emulator}")
        }
    }

    private suspend fun discoverPpsspp(
        root: RootEntity,
        rootRef: RootRef,
        rootDocumentId: String,
        reader: com.retrosave.core.adapters.azahar.SafTreeReader,
    ): RootDiscoveryResult {
        val candidates = discoverPpssppCandidates(rootDocumentId, reader)
        val units =
            candidates.directories.mapNotNull { candidate ->
                val unit =
                    UnitRef(
                        emulator = "ppsspp",
                        unitKey = candidate.name,
                        unitType = "dir",
                        gameKey = candidate.name.take(9),
                        // M8 §3 : le vrai titre est déjà dans le PARAM.SFO du
                        // dossier qu'on synchronise. Le serial ne reste que
                        // s'il est illisible.
                        gameLabel = ppsspLabel(rootRef, candidate.relPath, candidate.name),
                        root = rootRef,
                        relPath = candidate.relPath,
                    )
                discoveredDirectory(unit)
            }
        return RootDiscoveryResult(
            units = units,
            target =
                RootTarget(
                    emulator = root.emulator,
                    root = rootRef,
                    ppssppPrefix = candidates.effectivePrefix,
                ),
            status =
                AdapterRootStatus(
                    rootId = root.rootId,
                    emulator = root.emulator,
                    accessible = true,
                    unitCount = units.size,
                    warning =
                        if (candidates.effectivePrefix.isNotEmpty()) {
                            "PSP root detected; SAVEDATA is used automatically."
                        } else {
                            null
                        },
                ),
        )
    }

    private fun discoverMelonDs(
        root: RootEntity,
        rootRef: RootRef,
        rootDocumentId: String,
        reader: com.retrosave.core.adapters.azahar.SafTreeReader,
    ): RootDiscoveryResult {
        val maxDepth = specs.getValue("melonds").discovery.maxDepth
        val candidates = discoverMelonDsCandidates(rootDocumentId, reader, maxDepth)
        val units =
            candidates.saves.map { candidate ->
                DiscoveredUnit(
                    unit =
                        UnitRef(
                            emulator = "melonds",
                            unitKey = candidate.name,
                            unitType = "file",
                            gameKey = normalizeGameName(candidate.name),
                            gameLabel = displayName(candidate.name),
                            root = rootRef,
                            relPath = candidate.relPath,
                        ),
                    nodes =
                        listOf(
                            NodeMeta(
                                relPath = candidate.name,
                                sizeBytes = candidate.sizeBytes,
                                mtimeMs = candidate.mtimeMs,
                            ),
                        ),
                )
            }
        return RootDiscoveryResult(
            units = units,
            target =
                RootTarget(
                    emulator = root.emulator,
                    root = rootRef,
                    romRelPaths = candidates.romRelPaths,
                ),
            status =
                AdapterRootStatus(
                    rootId = root.rootId,
                    emulator = root.emulator,
                    accessible = true,
                    unitCount = units.size,
                ),
        )
    }

    private suspend fun discoverAzahar(
        root: RootEntity,
        rootRef: RootRef,
        rootDocumentId: String,
        reader: com.retrosave.core.adapters.azahar.SafTreeReader,
    ): RootDiscoveryResult {
        return when (
            val result =
                azaharDetector.detect(
                    rootDocumentId = rootDocumentId,
                    reader = reader,
                    activeIdentity = root.azaharIdentity,
                )
        ) {
            AzaharDetectionResult.InvalidRoot ->
                RootDiscoveryResult(
                    units = emptyList(),
                    target = RootTarget(emulator = root.emulator, root = rootRef),
                    status =
                        AdapterRootStatus(
                            rootId = root.rootId,
                            emulator = root.emulator,
                            accessible = true,
                            unitCount = 0,
                            warning = "This root has no Azahar markers yet.",
                        ),
                )
            is AzaharDetectionResult.IdentityChoiceRequired ->
                throw AzaharIdentityChoiceException(
                    rootId = root.rootId,
                    identities = result.identities,
                    proposedIdentity = result.proposedIdentity,
                )
            is AzaharDetectionResult.Valid -> {
                if (
                    result.activeIdentity != null &&
                    result.activeIdentity != root.azaharIdentity
                ) {
                    roots.upsert(root.copy(azaharIdentity = result.activeIdentity))
                }
                val units =
                    result.units.mapNotNull { detected ->
                        val identity = result.activeIdentity ?: return@mapNotNull null
                        val relPath =
                            "sdmc/Nintendo 3DS/$identity/title/00040000/" +
                                "${detected.titleIdLow}/data"
                        discoveredDirectory(
                            UnitRef(
                                emulator = "azahar",
                                unitKey = detected.unitKey,
                                unitType = "dir",
                                gameKey = detected.gameKey,
                                gameLabel = detected.gameLabel,
                                root = rootRef,
                                relPath = relPath,
                            ),
                        )
                    }
                RootDiscoveryResult(
                    units = units,
                    target =
                        RootTarget(
                            emulator = root.emulator,
                            root = rootRef,
                            azaharIdentity = result.activeIdentity,
                        ),
                    status =
                        AdapterRootStatus(
                            rootId = root.rootId,
                            emulator = root.emulator,
                            accessible = true,
                            unitCount = units.size,
                            activeIdentity = result.activeIdentity,
                            ignoredIdentities = result.ignoredIdentities,
                        ),
                )
            }
        }
    }

    /**
     * Titre lu dans `PARAM.SFO`, ou le serial si quoi que ce soit cloche.
     *
     * M8 §10 : un libellé ne fait jamais échouer une passe. Toute la chaîne —
     * ouverture SAF, lecture, décodage — retombe sur le serial sans bruit, y
     * compris quand le fichier est absent, ce qui est le cas le plus fréquent
     * sur un dossier de sauvegarde incomplet.
     */
    private suspend fun ppsspLabel(
        root: RootRef,
        relPath: String,
        name: String,
    ): String {
        val serial = name.take(9)
        val raw =
            try {
                vfs.readBytes(root, "${relPath.trimEnd('/')}/PARAM.SFO")
            } catch (_: Exception) {
                return serial
            }
        if (raw.size > MAX_SFO_BYTES) return serial
        val parsed = parseParamSfo(raw) ?: return serial
        return parsed.title?.takeIf { it.isNotBlank() } ?: serial
    }

    private suspend fun discoveredDirectory(unit: UnitRef): DiscoveredUnit? =
        try {
            val prefix = "${unit.relPath.trimEnd('/')}/"
            val nodes =
                vfs.listFiles(unit.root, unit.relPath).map {
                    it.copy(relPath = it.relPath.removePrefix(prefix))
                }
            if (nodes.isEmpty()) null else DiscoveredUnit(unit, nodes)
        } catch (error: UnsupportedNodeException) {
            DiscoveredUnit(unit, emptyList(), error.message)
        }

    private fun ensureReadable(
        root: RootEntity,
        uri: Uri,
    ) {
        val readable =
            contentResolver.persistedUriPermissions.any {
                it.uri == uri && it.isReadPermission
            }
        if (!readable) throw RootInaccessibleException(root.rootId)
    }

    private fun String.pointsInsideAndroidData(): Boolean {
        val normalized = replace('\\', '/').lowercase()
        return normalized.contains(":android/data") ||
            normalized.startsWith("android/data")
    }

    private data class RootDiscoveryResult(
        val units: List<DiscoveredUnit>,
        val target: RootTarget,
        val status: AdapterRootStatus,
    )

    private data class RootTarget(
        val emulator: String,
        val root: RootRef,
        val ppssppPrefix: String = "",
        val azaharIdentity: String? = null,
        val romRelPaths: List<String> = emptyList(),
    ) {
        fun ppssppTarget(remote: RemoteUnit): UnitRef? {
            if (remote.unitType != "dir" || !safeName(remote.unitKey)) return null
            return remote.toUnit(
                root = root,
                relPath = join(ppssppPrefix, remote.unitKey),
            )
        }

        fun melonDsTarget(remote: RemoteUnit): UnitRef? {
            if (
                remote.unitType != "file" ||
                !safeName(remote.unitKey) ||
                !remote.unitKey.endsWith(".sav", ignoreCase = true)
            ) {
                return null
            }
            val directory = melonDsRomDirectory(remote.unitKey, romRelPaths) ?: return null
            return remote.toUnit(root = root, relPath = join(directory, remote.unitKey))
        }

        fun azaharTarget(remote: RemoteUnit): UnitRef? {
            val identity = azaharIdentity ?: return null
            val titleId = remote.unitKey.lowercase()
            if (
                remote.unitType != "dir" ||
                titleId.length != 8 ||
                titleId.any { it !in "0123456789abcdef" }
            ) {
                return null
            }
            return remote.toUnit(
                root = root,
                relPath =
                    "sdmc/Nintendo 3DS/$identity/title/00040000/$titleId/data",
            )
        }

        private fun RemoteUnit.toUnit(
            root: RootRef,
            relPath: String,
        ) = UnitRef(
            emulator = emulator,
            unitKey = unitKey,
            unitType = unitType,
            gameKey = gameKey,
            gameLabel = gameLabel,
            root = root,
            relPath = relPath,
        )

        private companion object {
            fun safeName(name: String): Boolean =
                name.isNotEmpty() &&
                    name !in setOf(".", "..") &&
                    '/' !in name &&
                    '\\' !in name

            fun join(
                parent: String,
                child: String,
            ): String = if (parent.isEmpty()) child else "$parent/$child"
        }
    }

    private data class TargetSnapshot(
        val existing: Map<Pair<String, String>, UnitRef> = emptyMap(),
        val rootsByEmulator: Map<String, List<RootTarget>> = emptyMap(),
    )
}
