package com.retrosave.data.adapters

import android.net.Uri
import android.provider.DocumentsContract
import com.retrosave.data.db.daos.RootDao
import com.retrosave.data.db.entities.RootEntity
import java.nio.charset.StandardCharsets
import java.util.UUID

class RootRepository(
    private val roots: RootDao,
    private val activeAdapters: Set<String>,
) {
    suspend fun register(
        emulator: String,
        treeUri: Uri,
        label: String,
    ): RootEntity {
        require(emulator in activeAdapters) {
            "Adaptateur Android inactif : $emulator"
        }
        val documentId = DocumentsContract.getTreeDocumentId(treeUri)
        require(!documentId.pointsInsideAndroidData()) {
            "Android/data is out of scope."
        }
        val existing = roots.listByEmulator(emulator)
        check(
            existing.isEmpty() ||
                existing.all { it.treeUri == treeUri.toString() },
        ) {
            "A $emulator root is already configured. " +
                "Replacement stays blocked until Q12 is settled."
        }
        val previous = existing.firstOrNull()
        val root =
            RootEntity(
                rootId =
                    UUID
                        .nameUUIDFromBytes(
                            "$emulator|$treeUri".toByteArray(StandardCharsets.UTF_8),
                        ).toString(),
                emulator = emulator,
                treeUri = treeUri.toString(),
                label = label.ifBlank { emulator },
                azaharIdentity = previous?.azaharIdentity,
            )
        roots.upsert(root)
        return root
    }

    suspend fun selectAzaharIdentity(
        rootId: String,
        identity: String,
    ) {
        val root = requireNotNull(roots.find(rootId)) { "Racine inconnue." }
        require(root.emulator == "azahar")
        val components = identity.lowercase().split('/')
        require(
            components.size == 2 &&
                components.all { component ->
                    component.length == 32 &&
                        component.all { it in "0123456789abcdef" }
                },
        ) {
            "Invalid Azahar identity."
        }
        roots.upsert(root.copy(azaharIdentity = components.joinToString("/")))
    }

    private fun String.pointsInsideAndroidData(): Boolean {
        val normalized = replace('\\', '/').lowercase()
        return normalized.contains(":android/data") ||
            normalized.startsWith("android/data")
    }
}
