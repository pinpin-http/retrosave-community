package com.retrosave.core.sync

import com.retrosave.core.archive.ContentFile
import com.retrosave.core.archive.createArchive
import com.retrosave.core.fs.JvmVfs
import com.retrosave.core.model.DiscoveredUnit
import com.retrosave.core.model.LocalUnit
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import com.retrosave.core.model.UnitRef
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.nio.file.Files
import java.util.concurrent.atomic.AtomicLong

class SyncEngineTest {
    @get:Rule
    val temporary = TemporaryFolder()

    @Test
    fun `stable new unit is captured and pushed`() =
        runTest {
            val fixture = Fixture()
            fixture.write("slot-a/save.bin", "local-save".encodeToByteArray())
            fixture.discovered += fixture.discoverDirectory("slot-a")

            fixture.engine.scan()
            fixture.clock.set(10_000)
            val report = fixture.engine.sync()

            assertEquals(1, report.pushed)
            assertEquals(0, report.errors)
            assertNotNull(fixture.api.uploaded)
            val local = fixture.store.getUnit("folder", "slot-a")
            assertEquals(1, local?.lastSyncedVersion)
            assertEquals(fixture.api.confirmedContentSha256, local?.lastSyncedContentSha256)
        }

    @Test
    fun `remote head is restored into an empty local target`() =
        runTest {
            val fixture = Fixture()
            val archive =
                createArchive(
                    unitType = "dir",
                    files =
                        listOf(
                            ContentFile(
                                relPath = "save.bin",
                                content = "remote-save".encodeToByteArray(),
                            ),
                        ),
                )
            fixture.api.remoteUnits +=
                RemoteUnit(
                    id = "remote-slot",
                    emulator = "folder",
                    unitKey = "slot-b",
                    unitType = "dir",
                    gameKey = "folder:slot-b",
                    gameLabel = "slot-b",
                    headVersion = 3,
                    state = "active",
                    head =
                        RemoteHead(
                            number = 3,
                            contentSha256 = archive.contentSha256,
                            sizeBytes = archive.sizeBytes,
                        ),
                )
            fixture.api.download =
                DownloadTarget(
                    url = "memory://download",
                    archiveSha256 = archive.archiveSha256,
                    contentSha256 = archive.contentSha256,
                    archiveBytes = archive.archiveBytes,
                )
            fixture.api.downloadedArchive = archive.bytes

            val report = fixture.engine.sync()

            assertEquals(1, report.pulled)
            assertEquals(0, report.errors)
            assertArrayEquals(
                "remote-save".encodeToByteArray(),
                Files.readAllBytes(fixture.rootPath.resolve("slot-b/save.bin")),
            )
            assertEquals(
                3,
                fixture.store.getUnit("folder", "slot-b")?.lastSyncedVersion,
            )
        }

    @Test
    fun `CAS conflict preserves the uploaded branch identity`() =
        runTest {
            val fixture = Fixture()
            fixture.write("slot-a/save.bin", "local-branch".encodeToByteArray())
            fixture.discovered += fixture.discoverDirectory("slot-a")
            fixture.api.confirmConflict =
                CasConflictException(
                    conflictId = "conflict-1",
                    head =
                        ConflictHead(
                            number = 2,
                            contentSha256 = "a".repeat(64),
                            sizeBytes = 10,
                        ),
                    yours =
                        ConflictHead(
                            number = 3,
                            contentSha256 = "b".repeat(64),
                            sizeBytes = 12,
                        ),
                )

            fixture.engine.scan()
            fixture.clock.set(10_000)
            val report = fixture.engine.sync()

            assertEquals(1, report.conflicts)
            val local = fixture.store.getUnit("folder", "slot-a")
            assertEquals("conflict-1", local?.openConflictId)
            assertEquals(3, local?.pendingBranchNumber)
            assertEquals("b".repeat(64), local?.pendingBranchContentSha256)
        }

    @Test
    fun `landed write closes the apply journal without downloading`() =
        runTest {
            val fixture = Fixture()
            val content = "content-B".encodeToByteArray()
            fixture.write("slot-a/save.bin", content)
            fixture.discovered += fixture.discoverDirectory("slot-a")
            fixture.engine.scan()
            val local = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            val archive =
                createArchive(
                    unitType = "dir",
                    files = listOf(ContentFile(relPath = "save.bin", content = content)),
                )
            // Le crash a frappé entre l'écriture et lastSynced : le disque est déjà à jour.
            fixture.store.setApplyJournal(local.localId, 2, archive.contentSha256, 1_000)

            val report = fixture.engine.sync()

            val resumed = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            assertEquals(2, resumed.lastSyncedVersion)
            assertNull(resumed.applyJournalContentSha256)
            assertNull(fixture.api.download)
            assertEquals(0, report.pushed)
            assertEquals(0, report.conflicts)
        }

    @Test
    fun `torn content is reapplied by the pull without a second backup`() =
        runTest {
            val fixture = Fixture()
            fixture.write("slot-a/save.bin", "content-B-tor".encodeToByteArray())
            fixture.discovered += fixture.discoverDirectory("slot-a")
            fixture.engine.scan()
            val local = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            fixture.store.bindServer(local.localId, "remote-slot")
            val archive =
                createArchive(
                    unitType = "dir",
                    files =
                        listOf(
                            ContentFile(
                                relPath = "save.bin",
                                content = "content-B".encodeToByteArray(),
                            ),
                        ),
                )
            // Depuis Q19 la reprise ne télécharge plus : c'est le PULL qui
            // applique, donc l'unité doit exister côté serveur.
            fixture.api.remoteUnits +=
                RemoteUnit(
                    id = "remote-slot",
                    emulator = "folder",
                    unitKey = "slot-a",
                    unitType = "dir",
                    gameKey = "folder:slot-a",
                    gameLabel = "slot-a",
                    headVersion = 2,
                    state = "active",
                    head =
                        RemoteHead(
                            number = 2,
                            contentSha256 = archive.contentSha256,
                            sizeBytes = archive.sizeBytes,
                        ),
                )
            fixture.store.setApplyJournal(local.localId, 2, archive.contentSha256, 1_000)
            fixture.api.download =
                DownloadTarget(
                    url = "memory://resume",
                    archiveSha256 = archive.archiveSha256,
                    contentSha256 = archive.contentSha256,
                    archiveBytes = archive.archiveBytes,
                )
            fixture.api.downloadedArchive = archive.bytes
            // L'état d'avant le pull, que la reprise ne doit jamais écraser.
            fixture.write("slot-a.rsc-bak/save.bin", "content-A".encodeToByteArray())

            fixture.engine.sync()

            assertArrayEquals(
                "content-B".encodeToByteArray(),
                Files.readAllBytes(fixture.rootPath.resolve("slot-a/save.bin")),
            )
            assertArrayEquals(
                "content-A".encodeToByteArray(),
                Files.readAllBytes(fixture.rootPath.resolve("slot-a.rsc-bak/save.bin")),
            )
            val resumed = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            assertEquals(2, resumed.lastSyncedVersion)
            assertNull(resumed.applyJournalContentSha256)
        }

    @Test
    fun `a marked unit is never pushed`() =
        runTest {
            val fixture = Fixture()
            fixture.write("slot-a/save.bin", "content-B-tor".encodeToByteArray())
            fixture.discovered += fixture.discoverDirectory("slot-a")
            fixture.engine.scan()
            val local = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            // Marqueur sans serverId : la reprise ne peut rien retélécharger.
            fixture.store.setApplyJournal(local.localId, 2, "a".repeat(64), 1_000)

            fixture.clock.set(10_000)
            val report = fixture.engine.sync()

            assertNull(fixture.api.uploaded)
            assertEquals(0, report.pushed)
            val still = checkNotNull(fixture.store.getUnit("folder", "slot-a"))
            assertEquals("a".repeat(64), still.applyJournalContentSha256)
        }

    private inner class Fixture {
        val rootPath = temporary.newFolder().toPath()
        private val root = RootRef("root")
        val clock = AtomicLong(0)
        val store = MemoryUnitStore()
        val api = FakeApi()
        val discovered = mutableListOf<DiscoveredUnit>()
        private val vfs = JvmVfs(mapOf(root.rootId to rootPath))
        val engine =
            SyncEngine(
                vfs = vfs,
                api = api,
                store = store,
                discovery = UnitDiscovery { discovered.toList() },
                configuredRoots = { mapOf("folder" to listOf(root)) },
                remoteTarget =
                    RemoteTargetResolver {
                        UnitRef(
                            emulator = it.emulator,
                            unitKey = it.unitKey,
                            unitType = it.unitType,
                            gameKey = it.gameKey,
                            gameLabel = it.gameLabel,
                            root = root,
                            relPath = it.unitKey,
                        )
                    },
                clock = Clock(clock::get),
                sleeper = Sleeper {},
            )

        fun write(
            relPath: String,
            content: ByteArray,
        ) {
            val path = rootPath.resolve(relPath)
            Files.createDirectories(path.parent)
            Files.write(path, content)
            Files.setLastModifiedTime(
                path,
                java.nio.file.attribute.FileTime
                    .fromMillis(1),
            )
        }

        suspend fun discoverDirectory(relPath: String): DiscoveredUnit {
            val prefix = "$relPath/"
            val nodes =
                vfs.listFiles(root, relPath).map {
                    it.copy(relPath = it.relPath.removePrefix(prefix))
                }
            return DiscoveredUnit(
                unit =
                    UnitRef(
                        emulator = "folder",
                        unitKey = relPath,
                        unitType = "dir",
                        gameKey = "folder:$relPath",
                        gameLabel = relPath,
                        root = root,
                        relPath = relPath,
                    ),
                nodes = nodes,
            )
        }
    }
}

private class MemoryUnitStore : UnitStore {
    private val units = linkedMapOf<Pair<String, String>, LocalUnit>()
    private val fingerprints = mutableMapOf<Long, List<NodeMeta>>()
    private var nextId = 1L

    override suspend fun listUnits(emulator: String?): List<LocalUnit> =
        units.values
            .filter { emulator == null || it.emulator == emulator }
            .sortedWith(compareBy(LocalUnit::emulator, LocalUnit::unitKey))

    override suspend fun getUnit(
        emulator: String,
        unitKey: String,
    ): LocalUnit? = units[emulator to unitKey]

    override suspend fun getUnitByServerId(serverId: String): LocalUnit? = units.values.firstOrNull { it.serverId == serverId }

    override suspend fun upsertScan(
        unit: UnitRef,
        qfHash: String,
        nodes: List<NodeMeta>,
        now: Long,
    ): Pair<LocalUnit, Boolean> {
        val key = unit.emulator to unit.unitKey
        val existing = units[key]
        val changed = existing == null || existing.observedQfHash != qfHash
        val updated =
            if (existing == null) {
                LocalUnit(
                    localId = nextId++,
                    serverId = null,
                    emulator = unit.emulator,
                    unitKey = unit.unitKey,
                    unitType = unit.unitType,
                    gameKey = unit.gameKey,
                    gameLabel = unit.gameLabel,
                    rootId = unit.root.rootId,
                    relPath = unit.relPath,
                    observedQfHash = qfHash,
                    observedAt = now,
                    stableQfHash = qfHash,
                    stableSince = now,
                    lastSyncedVersion = 0,
                    lastSyncedContentSha256 = null,
                    state = "active",
                    openConflictId = null,
                    pendingBranchNumber = null,
                    pendingBranchContentSha256 = null,
                )
            } else {
                existing.copy(
                    unitType = unit.unitType,
                    gameKey = unit.gameKey,
                    gameLabel = unit.gameLabel,
                    rootId = unit.root.rootId,
                    relPath = unit.relPath,
                    observedQfHash = if (changed) qfHash else existing.observedQfHash,
                    observedAt = if (changed) now else existing.observedAt,
                    stableQfHash = if (changed) qfHash else existing.stableQfHash,
                    stableSince = if (changed) now else existing.stableSince,
                    state = "active",
                )
            }
        units[key] = updated
        fingerprints[updated.localId] = nodes
        return updated to changed
    }

    override suspend fun addRemotePlaceholder(
        unit: UnitRef,
        serverId: String,
    ): LocalUnit {
        val key = unit.emulator to unit.unitKey
        val existing = units[key]
        val updated =
            existing?.copy(serverId = serverId, gameLabel = unit.gameLabel)
                ?: LocalUnit(
                    localId = nextId++,
                    serverId = serverId,
                    emulator = unit.emulator,
                    unitKey = unit.unitKey,
                    unitType = unit.unitType,
                    gameKey = unit.gameKey,
                    gameLabel = unit.gameLabel,
                    rootId = unit.root.rootId,
                    relPath = unit.relPath,
                    observedQfHash = null,
                    observedAt = null,
                    stableQfHash = null,
                    stableSince = null,
                    lastSyncedVersion = 0,
                    lastSyncedContentSha256 = null,
                    state = "active",
                    openConflictId = null,
                    pendingBranchNumber = null,
                    pendingBranchContentSha256 = null,
                )
        units[key] = updated
        return updated
    }

    override suspend fun markMissingExcept(
        emulator: String,
        rootId: String,
        discoveredKeys: Set<String>,
    ): List<LocalUnit> {
        val missing =
            units.values.filter {
                it.emulator == emulator &&
                    it.rootId == rootId &&
                    it.unitKey !in discoveredKeys &&
                    it.state == "active" &&
                    it.observedQfHash != null
            }
        missing.forEach { replace(it.copy(state = "missing")) }
        return missing
    }

    override suspend fun bindServer(
        localId: Long,
        serverId: String,
    ) = update(localId) { it.copy(serverId = serverId) }

    override suspend fun markUnsupported(localId: Long) = update(localId) { it.copy(state = "unsupported") }

    override suspend fun markDuplicate(localId: Long) = update(localId) { it.copy(state = "duplicate") }

    override suspend fun pauseAll() {
        units.values.toList().forEach { replace(it.copy(state = "paused")) }
    }

    override suspend fun markError(localId: Long) {
        update(localId) { it.copy(state = "error") }
    }

    override suspend fun recordFailure(
        localId: Long,
        headVersion: Int?,
    ) {
        update(localId) {
            it.copy(
                consecutiveFailures = it.consecutiveFailures + 1,
                lastFailureHead = headVersion,
            )
        }
    }

    override suspend fun clearFailures(localId: Long) {
        update(localId) { it.copy(consecutiveFailures = 0, lastFailureHead = null) }
    }

    override suspend fun adoptRemoteLabel(
        localId: Long,
        label: String,
        source: String,
    ) {
        update(localId) { it.copy(gameLabel = label, labelSource = source) }
    }

    override suspend fun resumeUnit(localId: Long) {
        update(localId) {
            if (it.state == "error" || it.state == "paused") it.copy(state = "active") else it
        }
    }

    override suspend fun resumeAll() {
        units.values
            .filter { it.state == "paused" }
            .forEach { replace(it.copy(state = "active")) }
    }

    override suspend fun setSynced(
        localId: Long,
        version: Int,
        contentSha256: String,
        qfHash: String?,
        now: Long?,
        clearPending: Boolean,
    ) = update(localId) {
        it.copy(
            lastSyncedVersion = version,
            lastSyncedContentSha256 = contentSha256,
            state = "active",
            openConflictId = null,
            observedQfHash = qfHash ?: it.observedQfHash,
            observedAt = if (qfHash != null) now else it.observedAt,
            stableQfHash = qfHash ?: it.stableQfHash,
            stableSince = if (qfHash != null) now else it.stableSince,
            pendingBranchNumber =
                if (clearPending) null else it.pendingBranchNumber,
            pendingBranchContentSha256 =
                if (clearPending) null else it.pendingBranchContentSha256,
            applyJournalVersion = null,
            applyJournalContentSha256 = null,
            applyJournalStartedAt = null,
        )
    }

    override suspend fun setConflict(
        localId: Long,
        conflictId: String,
        pendingBranchNumber: Int?,
        pendingBranchContentSha256: String?,
    ) = update(localId) {
        it.copy(
            openConflictId = conflictId,
            pendingBranchNumber = pendingBranchNumber,
            pendingBranchContentSha256 = pendingBranchContentSha256,
        )
    }

    override suspend fun clearOpenConflict(localId: Long) = update(localId) { it.copy(openConflictId = null) }

    override suspend fun setApplyJournal(
        localId: Long,
        version: Int,
        contentSha256: String,
        now: Long,
    ) = update(localId) {
        it.copy(
            applyJournalVersion = version,
            applyJournalContentSha256 = contentSha256,
            applyJournalStartedAt = now,
        )
    }

    override suspend fun clearApplyJournal(localId: Long) =
        update(localId) {
            it.copy(
                applyJournalVersion = null,
                applyJournalContentSha256 = null,
                applyJournalStartedAt = null,
            )
        }

    override suspend fun fingerprints(localId: Long): List<NodeMeta> = fingerprints[localId].orEmpty()

    override suspend fun journal(
        now: Long,
        event: String,
        detail: String,
        localId: Long?,
    ) = Unit

    private fun update(
        localId: Long,
        transform: (LocalUnit) -> LocalUnit,
    ) {
        replace(transform(units.values.first { it.localId == localId }))
    }

    private fun replace(unit: LocalUnit) {
        units[unit.emulator to unit.unitKey] = unit
    }
}

private class FakeApi : SyncApi {
    val remoteUnits = mutableListOf<RemoteUnit>()
    var uploaded: ByteArray? = null
    var confirmedContentSha256: String? = null
    var confirmConflict: CasConflictException? = null
    var download: DownloadTarget? = null
    var downloadedArchive: ByteArray = byteArrayOf()

    override suspend fun health(): Boolean = true

    override suspend fun registerDevice(
        name: String,
        osName: String,
        appVersion: String,
    ): String = "device"

    override suspend fun listUnits(): List<RemoteUnit> = remoteUnits

    override suspend fun createUnit(
        unit: UnitRef,
        idempotencyKey: String,
    ): RemoteUnit =
        RemoteUnit(
            id = "server-${unit.unitKey}",
            emulator = unit.emulator,
            unitKey = unit.unitKey,
            unitType = unit.unitType,
            gameKey = unit.gameKey,
            gameLabel = unit.gameLabel,
            headVersion = 0,
            state = "active",
            head = null,
        ).also(remoteUnits::add)

    override suspend fun prepareVersion(
        unitId: String,
        baseVersion: Int,
        contentSha256: String,
        archiveSha256: String,
        size: Long,
        archiveBytes: Long,
        idempotencyKey: String,
    ): PrepareResult =
        PrepareResult(
            upload =
                UploadTarget(
                    url = "memory://upload",
                    objectKey = "u/user/$unitId/$contentSha256.tar.zst",
                ),
        )

    override suspend fun uploadArchive(
        url: String,
        archive: ByteArray,
    ) {
        uploaded = archive
    }

    override suspend fun upload(
        url: String,
        source: java.io.File,
    ) {
        uploaded = source.readBytes()
    }

    override suspend fun confirmVersion(
        unitId: String,
        objectKey: String,
        contentSha256: String,
        archiveSha256: String,
        baseVersion: Int,
        environment: Map<String, String>,
        clientMtime: String?,
        idempotencyKey: String,
    ): Int {
        confirmConflict?.let { throw it }
        confirmedContentSha256 = contentSha256
        return 1
    }

    override suspend fun downloadVersion(
        unitId: String,
        number: Int,
    ): DownloadTarget = requireNotNull(download)

    override suspend fun downloadArchive(url: String): ByteArray = downloadedArchive

    override suspend fun download(
        url: String,
        dest: java.io.File,
    ): String {
        dest.parentFile?.mkdirs()
        dest.writeBytes(downloadedArchive)
        return java.security.MessageDigest
            .getInstance("SHA-256")
            .digest(downloadedArchive)
            .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    override suspend fun markMissing(
        unitId: String,
        idempotencyKey: String,
    ) = Unit

    override suspend fun listVersions(unitId: String): Pair<List<RemoteVersion>, Int> = emptyList<RemoteVersion>() to 0

    override suspend fun restore(
        unitId: String,
        number: Int,
        idempotencyKey: String,
    ): Int = number

    override suspend fun listConflicts(): List<RemoteConflict> = emptyList()

    override suspend fun resolveConflict(
        conflictId: String,
        winner: Int,
        idempotencyKey: String,
    ): Pair<String, Int> = "unit" to winner
}
