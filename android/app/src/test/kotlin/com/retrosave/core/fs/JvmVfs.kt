package com.retrosave.core.fs

import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import java.io.FileOutputStream
import java.io.InputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.Comparator

class JvmVfs(
    roots: Map<String, Path>,
) : Vfs {
    private val roots = roots.mapValues { it.value.toRealPath() }

    override suspend fun listFiles(
        root: RootRef,
        subPath: String,
    ): List<NodeMeta> {
        val start = path(root, subPath)
        if (Files.isRegularFile(start)) {
            return listOf(meta(root, start))
        }
        require(Files.isDirectory(start)) { "Not a directory: $subPath" }
        val nodes = mutableListOf<NodeMeta>()
        Files.walk(start).use { paths ->
            paths.forEach { candidate ->
                if (Files.isSymbolicLink(candidate)) {
                    throw UnsupportedNodeException("symbolic link inside unit")
                }
                when {
                    Files.isDirectory(candidate) -> Unit
                    Files.isRegularFile(candidate) &&
                        !isInternal(candidate.fileName.toString()) -> {
                        nodes += meta(root, candidate)
                    }
                    Files.isRegularFile(candidate) -> Unit
                    else -> throw UnsupportedNodeException("unsupported filesystem node")
                }
            }
        }
        return nodes.sortedBy(NodeMeta::relPath)
    }

    override suspend fun readBytes(
        root: RootRef,
        relPath: String,
    ): ByteArray = Files.readAllBytes(requireFile(root, relPath))

    override suspend fun openRead(
        root: RootRef,
        relPath: String,
    ): InputStream = Files.newInputStream(requireFile(root, relPath)).buffered(BUFFER_SIZE)

    override suspend fun sha256(
        root: RootRef,
        relPath: String,
    ): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        Files.newInputStream(requireFile(root, relPath)).buffered(BUFFER_SIZE).use {
            val buffer = ByteArray(BUFFER_SIZE)
            while (true) {
                val count = it.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest()
    }

    override suspend fun writeAtomic(
        root: RootRef,
        relPath: String,
        source: InputStream,
        expectedSha256: String,
    ) {
        // Q27 : le backup est une politique de niveau unité — un jeu par
        // application, et jamais sous marqueur, règle que le Vfs ne peut pas
        // connaître. Il appartient au moteur ; `writeAtomic` n'en prend aucun.
        val target = path(root, relPath, requireExists = false)
        Files.createDirectories(target.parent)
        val temporary = target.resolveSibling("${target.fileName}.rsc-tmp")
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            FileOutputStream(temporary.toFile()).use { output ->
                val buffer = ByteArray(BUFFER_SIZE)
                while (true) {
                    val count = source.read(buffer)
                    if (count < 0) break
                    output.write(buffer, 0, count)
                    digest.update(buffer, 0, count)
                }
                output.flush()
                output.fd.sync()
            }
            // Relecture Q13 : ce que le disque rend, pas ce qu'on croit y avoir mis.
            check(digest.digest().joinToString("") { "%02x".format(it) } == expectedSha256) {
                "Written content verification failed for $relPath"
            }
            try {
                Files.move(
                    temporary,
                    target,
                    StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING,
                )
            } catch (_: AtomicMoveNotSupportedException) {
                Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            Files.deleteIfExists(temporary)
        }
    }

    override suspend fun delete(
        root: RootRef,
        relPath: String,
    ) {
        deleteTree(path(root, relPath))
    }

    override suspend fun backup(
        root: RootRef,
        relPath: String,
    ) {
        val source = path(root, relPath)
        val backup = source.resolveSibling("${source.fileName}.rsc-bak")
        val generationOne = backup.resolveSibling("${backup.fileName}.1")
        val generationTwo = backup.resolveSibling("${backup.fileName}.2")
        deleteTreeIfExists(generationTwo)
        if (Files.exists(generationOne)) {
            Files.move(generationOne, generationTwo)
        }
        if (Files.exists(backup)) {
            Files.move(backup, generationOne)
        }
        copyTree(source, backup)
    }

    override suspend fun exists(
        root: RootRef,
        relPath: String,
    ): Boolean = Files.exists(path(root, relPath, requireExists = false))

    private fun requireFile(
        root: RootRef,
        relPath: String,
    ): Path =
        path(root, relPath).also {
            require(Files.isRegularFile(it) && !Files.isSymbolicLink(it)) {
                "Not a regular file: $relPath"
            }
        }

    private fun path(
        root: RootRef,
        relPath: String,
        requireExists: Boolean = true,
    ): Path {
        require('\\' !in relPath)
        val components =
            if (relPath.isEmpty()) {
                emptyList()
            } else {
                relPath.split('/').also { parts ->
                    require(parts.none { it.isEmpty() || it == "." || it == ".." })
                }
            }
        val base = requireNotNull(roots[root.rootId])
        val target = components.fold(base) { current, component -> current.resolve(component) }
        val normalized = target.normalize()
        require(normalized.startsWith(base)) { "Path escapes root" }
        if (requireExists) {
            check(Files.exists(normalized)) { "Path does not exist: $relPath" }
        }
        return normalized
    }

    private fun meta(
        root: RootRef,
        path: Path,
    ) = NodeMeta(
        relPath = roots.getValue(root.rootId).relativize(path).joinToString("/"),
        sizeBytes = Files.size(path),
        mtimeMs = Files.getLastModifiedTime(path).toMillis(),
    )

    private fun copyTree(
        source: Path,
        target: Path,
    ) {
        if (Files.isRegularFile(source)) {
            Files.copy(source, target)
            return
        }
        Files.walk(source).use { paths ->
            paths.forEach {
                val destination = target.resolve(source.relativize(it))
                if (Files.isDirectory(it)) {
                    Files.createDirectories(destination)
                } else {
                    Files.copy(it, destination)
                }
            }
        }
    }

    private fun deleteTreeIfExists(path: Path) {
        if (Files.exists(path)) deleteTree(path)
    }

    private fun deleteTree(path: Path) {
        if (Files.isDirectory(path)) {
            Files.walk(path).use { paths ->
                paths.sorted(Comparator.reverseOrder()).forEach(Files::delete)
            }
        } else {
            Files.delete(path)
        }
    }

    private companion object {
        const val BUFFER_SIZE = 64 * 1024

        fun isInternal(name: String): Boolean =
            name.startsWith(".rsc-") ||
                name.endsWith(".rsc-tmp") ||
                name.endsWith(".rsc-bak") ||
                name.endsWith(".rsc-bak.1") ||
                name.endsWith(".rsc-bak.2")
    }
}
