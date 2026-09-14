package com.retrosave.data.fs

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import com.retrosave.core.fs.RENAME_DEVIATED
import com.retrosave.core.fs.RENAME_REFUSED
import com.retrosave.core.fs.RootInaccessibleException
import com.retrosave.core.fs.Vfs
import com.retrosave.core.fs.renameOutcome
import com.retrosave.core.model.NodeMeta
import com.retrosave.core.model.RootRef
import com.retrosave.data.db.daos.RootDao
import com.retrosave.data.db.entities.RootEntity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.InputStream
import java.security.MessageDigest
import java.util.ArrayDeque

private const val BUFFER_SIZE = 64 * 1024
private const val MIME_DIRECTORY = DocumentsContract.Document.MIME_TYPE_DIR

class SafVfs(
    context: Context,
    private val roots: RootDao,
) : Vfs {
    private val resolver = context.contentResolver

    override suspend fun listFiles(
        root: RootRef,
        subPath: String,
    ): List<NodeMeta> =
        onIo(root) { registered ->
            val start = resolve(registered, subPath)
            if (!start.isDirectory) {
                return@onIo listOf(start.toMeta(subPath))
            }

            val result = mutableListOf<NodeMeta>()
            val queue = ArrayDeque<QueuedDirectory>()
            queue.add(QueuedDirectory(start.documentId, subPath))
            while (queue.isNotEmpty()) {
                val current = queue.removeFirst()
                queryChildren(registered, current.documentId).forEach { child ->
                    if (!isInternalName(child.name)) {
                        val path = join(current.relPath, child.name)
                        if (child.isDirectory) {
                            queue.add(QueuedDirectory(child.documentId, path))
                        } else {
                            result += child.toMeta(path)
                        }
                    }
                }
            }
            result.sortedBy(NodeMeta::relPath)
        }

    override suspend fun readBytes(
        root: RootRef,
        relPath: String,
    ): ByteArray =
        onIo(root) { registered ->
            val node = requireFile(registered, relPath)
            openInput(registered, node).use { input ->
                val output = ByteArrayOutputStream()
                val buffer = ByteArray(BUFFER_SIZE)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    output.write(buffer, 0, count)
                }
                output.toByteArray()
            }
        }

    override suspend fun openRead(
        root: RootRef,
        relPath: String,
    ): InputStream =
        onIo(root) { registered ->
            openInput(registered, requireFile(registered, relPath))
        }

    override suspend fun sha256(
        root: RootRef,
        relPath: String,
    ): ByteArray =
        onIo(root) { registered ->
            hashDocument(registered, requireFile(registered, relPath))
        }

    override suspend fun writeAtomic(
        root: RootRef,
        relPath: String,
        source: InputStream,
        expectedSha256: String,
    ) {
        onIo(root) { registered ->
            val components = safeComponents(relPath)
            require(components.isNotEmpty()) { "A file path is required" }
            val name = components.last()
            val parentPath = components.dropLast(1).joinToString("/")
            val parent = ensureDirectory(registered, parentPath)
            require(parent.isDirectory) { "Parent is not a directory: $parentPath" }
            val existing = findChild(registered, parent.documentId, name)
            if (existing != null) {
                // AD-27 — remplacement : SAF n'a pas de rename atomique, on écrit EN PLACE.
                backupInternal(registered, relPath, existing)
                val backup = resolve(registered, "$relPath.rsc-bak")
                check(verifyCopy(registered, existing, backup)) {
                    "Backup verification failed before replacing $relPath"
                }
                if (writeInPlace(registered, existing, source)) {
                    check(hashDocument(registered, existing).toHexString() == expectedSha256) {
                        "Written content verification failed for $relPath"
                    }
                    return@onIo
                }
                // Repli documenté : le provider refuse "wt" ; le backup est vérifié.
                deleteDocument(registered, existing)
            }

            val temporaryName = "$name.rsc-tmp"
            findChild(registered, parent.documentId, temporaryName)?.let {
                deleteDocument(registered, it)
            }
            val temporary =
                createFile(
                    registered = registered,
                    parentId = parent.documentId,
                    name = temporaryName,
                    source = source,
                )
            try {
                val renamed = rename(registered, temporary, name)
                // AD-27 : un nom dévié (`nom (1).ext`) n'est PAS un succès — on le retire
                // et on échoue bruyamment plutôt que de laisser un doublon silencieux.
                when (renameOutcome(name, renamed?.name)) {
                    RENAME_DEVIATED -> {
                        renamed?.let { deleteDocument(registered, it) }
                        error("SAF provider renamed $relPath to ${renamed?.name} instead of $name")
                    }
                    RENAME_REFUSED -> error("SAF provider refused final rename for $relPath")
                }
                checkNotNull(renamed)
                check(hashDocument(registered, renamed).toHexString() == expectedSha256) {
                    "Written content verification failed for $relPath"
                }
            } finally {
                findChild(registered, parent.documentId, temporaryName)?.let {
                    deleteDocument(registered, it)
                }
            }
        }
    }

    override suspend fun delete(
        root: RootRef,
        relPath: String,
    ) {
        onIo(root) { registered ->
            deleteDocument(registered, resolve(registered, relPath))
        }
    }

    override suspend fun backup(
        root: RootRef,
        relPath: String,
    ) {
        onIo(root) { registered ->
            backupInternal(registered, relPath, resolve(registered, relPath))
        }
    }

    override suspend fun exists(
        root: RootRef,
        relPath: String,
    ): Boolean =
        onIo(root) { registered ->
            resolveOrNull(registered, relPath) != null
        }

    private fun backupInternal(
        root: RegisteredRoot,
        relPath: String,
        source: DocumentNode,
    ) {
        val components = safeComponents(relPath)
        val name = components.last()
        val parentPath = components.dropLast(1).joinToString("/")
        val parent = resolve(root, parentPath)
        val backupName = "$name.rsc-bak"
        val generationOne = "$backupName.1"
        val generationTwo = "$backupName.2"

        findChild(root, parent.documentId, generationTwo)?.let {
            deleteDocument(root, it)
        }
        findChild(root, parent.documentId, generationOne)?.let {
            move(root, parent, it, generationTwo)
        }
        findChild(root, parent.documentId, backupName)?.let {
            move(root, parent, it, generationOne)
        }

        val backup = copy(root, source, parent.documentId, backupName)
        check(verifyCopy(root, source, backup)) {
            "Backup verification failed for $relPath"
        }
    }

    private fun move(
        root: RegisteredRoot,
        parent: DocumentNode,
        source: DocumentNode,
        targetName: String,
    ) {
        val renamed = rename(root, source, targetName)
        if (renamed != null) return

        val copied = copy(root, source, parent.documentId, targetName)
        check(verifyCopy(root, source, copied)) {
            "Backup rotation verification failed"
        }
        deleteDocument(root, source)
    }

    private fun copy(
        root: RegisteredRoot,
        source: DocumentNode,
        targetParentId: String,
        targetName: String,
    ): DocumentNode {
        if (!source.isDirectory) {
            val uri =
                DocumentsContract.createDocument(
                    resolver,
                    documentUri(root, targetParentId),
                    "application/octet-stream",
                    targetName,
                ) ?: error("SAF provider refused file creation: $targetName")
            openInput(root, source).use { input ->
                resolver.openFileDescriptor(uri, "w")?.use { descriptor ->
                    FileOutputStream(descriptor.fileDescriptor).use { output ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            output.write(buffer, 0, count)
                        }
                        output.flush()
                        descriptor.fileDescriptor.sync()
                    }
                } ?: error("SAF provider refused file write: $targetName")
            }
            return requireNotNull(findChild(root, targetParentId, targetName)) {
                "Copied SAF file is not visible: $targetName"
            }
        }

        val directory = createDirectory(root, targetParentId, targetName)
        queryChildren(root, source.documentId)
            .filterNot { isInternalName(it.name) }
            .forEach {
                copy(root, it, directory.documentId, it.name)
            }
        return directory
    }

    private fun verifyCopy(
        root: RegisteredRoot,
        source: DocumentNode,
        target: DocumentNode,
    ): Boolean {
        if (source.isDirectory != target.isDirectory) return false
        if (!source.isDirectory) {
            if (source.sizeBytes != target.sizeBytes) return false
            return hashDocument(root, source).contentEquals(hashDocument(root, target))
        }
        val sourceChildren =
            queryChildren(root, source.documentId)
                .filterNot { isInternalName(it.name) }
                .associateBy(DocumentNode::name)
        val targetChildren =
            queryChildren(root, target.documentId)
                .filterNot { isInternalName(it.name) }
                .associateBy(DocumentNode::name)
        if (sourceChildren.keys != targetChildren.keys) return false
        return sourceChildren.all { (name, child) ->
            verifyCopy(root, child, targetChildren.getValue(name))
        }
    }

    private fun createFile(
        registered: RegisteredRoot,
        parentId: String,
        name: String,
        source: InputStream,
    ): DocumentNode {
        val parentUri = documentUri(registered, parentId)
        val uri =
            DocumentsContract.createDocument(
                resolver,
                parentUri,
                "application/octet-stream",
                name,
            ) ?: error("SAF provider refused file creation: $name")
        resolver.openFileDescriptor(uri, "w")?.use { descriptor ->
            FileOutputStream(descriptor.fileDescriptor).use { output ->
                source.copyTo(output, BUFFER_SIZE)
                output.flush()
                descriptor.fileDescriptor.sync()
            }
        } ?: error("SAF provider refused file write: $name")
        return requireNotNull(findChild(registered, parentId, name)) {
            "Created SAF file is not visible: $name"
        }
    }

    private fun createDirectory(
        registered: RegisteredRoot,
        parentId: String,
        name: String,
    ): DocumentNode {
        val uri =
            DocumentsContract.createDocument(
                resolver,
                documentUri(registered, parentId),
                MIME_DIRECTORY,
                name,
            ) ?: error("SAF provider refused directory creation: $name")
        val documentId = DocumentsContract.getDocumentId(uri)
        return DocumentNode(
            documentId = documentId,
            name = name,
            sizeBytes = 0,
            mtimeMs = null,
            mimeType = MIME_DIRECTORY,
        )
    }

    /**
     * Remplace le contenu d'un document existant sans le renommer (AD-27).
     *
     * Le mode `"wt"` est obligatoire : `"w"` seul ne tronque pas chez plusieurs
     * providers, ce qui laisserait des octets résiduels quand le nouveau contenu
     * est plus court. Retourne false si le provider refuse le mode.
     */
    private fun writeInPlace(
        root: RegisteredRoot,
        target: DocumentNode,
        source: InputStream,
    ): Boolean =
        try {
            // Le flux n'est consommé qu'une fois le descripteur ouvert : si le
            // provider refuse "wt", rien n'a été lu et le repli par fichier
            // temporaire peut encore s'en servir.
            resolver.openFileDescriptor(documentUri(root, target.documentId), "wt")?.use { fd ->
                FileOutputStream(fd.fileDescriptor).use { output ->
                    source.copyTo(output, BUFFER_SIZE)
                    output.flush()
                    fd.fileDescriptor.sync()
                }
                true
            } ?: false
        } catch (_: IllegalArgumentException) {
            false
        } catch (_: UnsupportedOperationException) {
            false
        } catch (_: FileNotFoundException) {
            false
        }

    private fun displayNameOf(uri: android.net.Uri): String? =
        resolver
            .query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }

    private fun rename(
        root: RegisteredRoot,
        source: DocumentNode,
        targetName: String,
    ): DocumentNode? =
        try {
            val uri =
                DocumentsContract.renameDocument(
                    resolver,
                    documentUri(root, source.documentId),
                    targetName,
                ) ?: return null
            // AD-27 : certains providers « réussissent » en suffixant (`nom (1).ext`).
            // Le nom obtenu est relu, jamais supposé égal au nom demandé.
            source.copy(
                documentId = DocumentsContract.getDocumentId(uri),
                name = displayNameOf(uri) ?: targetName,
            )
        } catch (_: FileNotFoundException) {
            null
        } catch (_: IllegalStateException) {
            null
        }

    private fun deleteDocument(
        root: RegisteredRoot,
        node: DocumentNode,
    ) {
        check(
            DocumentsContract.deleteDocument(
                resolver,
                documentUri(root, node.documentId),
            ),
        ) {
            "SAF provider refused deletion: ${node.name}"
        }
    }

    private fun hashDocument(
        root: RegisteredRoot,
        node: DocumentNode,
    ): ByteArray {
        val digest = MessageDigest.getInstance("SHA-256")
        openInput(root, node).use { input ->
            val buffer = ByteArray(BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest()
    }

    private fun openInput(
        root: RegisteredRoot,
        node: DocumentNode,
    ) = resolver.openInputStream(documentUri(root, node.documentId))
        ?: throw FileNotFoundException(node.name)

    private fun requireFile(
        root: RegisteredRoot,
        relPath: String,
    ): DocumentNode {
        val node = resolve(root, relPath)
        require(!node.isDirectory) { "Not a regular document: $relPath" }
        return node
    }

    private fun resolve(
        root: RegisteredRoot,
        relPath: String,
    ): DocumentNode =
        resolveOrNull(root, relPath)
            ?: throw FileNotFoundException(relPath)

    private fun resolveOrNull(
        root: RegisteredRoot,
        relPath: String,
    ): DocumentNode? {
        var node =
            DocumentNode(
                documentId = root.documentId,
                name = "",
                sizeBytes = 0,
                mtimeMs = null,
                mimeType = MIME_DIRECTORY,
            )
        for (component in safeComponents(relPath)) {
            if (!node.isDirectory) return null
            node = findChild(root, node.documentId, component) ?: return null
        }
        return node
    }

    private fun ensureDirectory(
        root: RegisteredRoot,
        relPath: String,
    ): DocumentNode {
        var node =
            DocumentNode(
                documentId = root.documentId,
                name = "",
                sizeBytes = 0,
                mtimeMs = null,
                mimeType = MIME_DIRECTORY,
            )
        safeComponents(relPath).forEachIndexed { index, component ->
            val existing = findChild(root, node.documentId, component)
            node =
                if (existing != null) {
                    require(existing.isDirectory) {
                        "Parent component is not a directory: $component"
                    }
                    existing
                } else {
                    check(
                        root.entity.emulator != "azahar" ||
                            index > AZAHAR_IDENTITY_LAST_INDEX,
                    ) {
                        "RetroSave never creates an Azahar identity."
                    }
                    createDirectory(root, node.documentId, component)
                }
        }
        return node
    }

    private fun findChild(
        root: RegisteredRoot,
        parentId: String,
        name: String,
    ): DocumentNode? = queryChildren(root, parentId).firstOrNull { it.name == name }

    private fun queryChildren(
        root: RegisteredRoot,
        parentId: String,
    ): List<DocumentNode> {
        val uri =
            DocumentsContract.buildChildDocumentsUriUsingTree(
                root.treeUri,
                parentId,
            )
        val cursor =
            resolver.query(uri, PROJECTION, null, null, null)
                ?: throw RootInaccessibleException(root.entity.rootId)
        return cursor.use {
            val idIndex = it.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            val nameIndex = it.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
            val sizeIndex = it.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_SIZE)
            val mtimeIndex = it.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_LAST_MODIFIED)
            val mimeIndex = it.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
            buildList {
                while (it.moveToNext()) {
                    add(
                        DocumentNode(
                            documentId = it.getString(idIndex),
                            name = it.getString(nameIndex),
                            sizeBytes = if (it.isNull(sizeIndex)) 0 else it.getLong(sizeIndex),
                            mtimeMs = if (it.isNull(mtimeIndex)) null else it.getLong(mtimeIndex),
                            mimeType = it.getString(mimeIndex),
                        ),
                    )
                }
            }
        }
    }

    private suspend fun <T> onIo(
        root: RootRef,
        block: (RegisteredRoot) -> T,
    ): T =
        withContext(Dispatchers.IO) {
            val entity =
                roots.find(root.rootId)
                    ?: throw RootInaccessibleException(root.rootId)
            val treeUri = Uri.parse(entity.treeUri)
            val registered =
                try {
                    RegisteredRoot(
                        entity = entity,
                        treeUri = treeUri,
                        documentId = DocumentsContract.getTreeDocumentId(treeUri),
                    )
                } catch (error: RuntimeException) {
                    throw RootInaccessibleException(root.rootId, error)
                }
            try {
                block(registered)
            } catch (error: SecurityException) {
                throw RootInaccessibleException(root.rootId, error)
            }
        }

    private fun documentUri(
        root: RegisteredRoot,
        documentId: String,
    ): Uri =
        DocumentsContract.buildDocumentUriUsingTree(
            root.treeUri,
            documentId,
        )

    private data class RegisteredRoot(
        val entity: RootEntity,
        val treeUri: Uri,
        val documentId: String,
    )

    private data class DocumentNode(
        val documentId: String,
        val name: String,
        val sizeBytes: Long,
        val mtimeMs: Long?,
        val mimeType: String,
    ) {
        val isDirectory: Boolean
            get() = mimeType == MIME_DIRECTORY

        fun toMeta(relPath: String) =
            NodeMeta(
                relPath = relPath,
                sizeBytes = sizeBytes,
                mtimeMs = mtimeMs,
            )
    }

    private data class QueuedDirectory(
        val documentId: String,
        val relPath: String,
    )

    private companion object {
        const val AZAHAR_IDENTITY_LAST_INDEX = 3

        val PROJECTION =
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            )

        fun safeComponents(relPath: String): List<String> {
            require('\\' !in relPath) { "Paths must use '/' separators" }
            if (relPath.isEmpty()) return emptyList()
            val components = relPath.split('/')
            require(components.none { it.isEmpty() || it == "." || it == ".." }) {
                "Unsafe relative path: $relPath"
            }
            return components
        }

        fun join(
            parent: String,
            child: String,
        ): String = if (parent.isEmpty()) child else "$parent/$child"

        fun isInternalName(name: String): Boolean =
            name.startsWith(".rsc-") ||
                name.endsWith(".rsc-tmp") ||
                name.endsWith(".rsc-bak") ||
                name.endsWith(".rsc-bak.1") ||
                name.endsWith(".rsc-bak.2")
    }
}

private fun ByteArray.toHexString(): String =
    joinToString("") { byte ->
        val value = byte.toInt() and 0xff
        "0123456789abcdef"[value shr 4].toString() + "0123456789abcdef"[value and 0x0f]
    }
