package com.retrosave.core.fs

import com.retrosave.core.model.RootRef
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.File
import java.security.MessageDigest

/**
 * Q27 — le backup appartient au moteur, `writeAtomic` n'en prend jamais.
 *
 * C'est une politique de niveau *unité* : un jeu `.rsc-bak` par application,
 * avec rotation, et surtout « jamais sous marqueur » — une règle que le `Vfs` ne
 * peut pas connaître sans franchir une couche. Un backup par fichier parasiterait
 * la rotation sur les unités multi-fichiers et doublerait celui du moteur en
 * silence.
 *
 * Cette assertion manquait au niveau JVM, et c'est précisément elle qui aurait
 * attrapé la divergence entre `JvmVfs` et `LocalFs`.
 */
class BackupOwnershipTest {
    @get:Rule
    val folder = TemporaryFolder()

    private fun sha256(content: ByteArray): String =
        MessageDigest
            .getInstance("SHA-256")
            .digest(content)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    @Test
    fun `writing a multi-file unit leaves no backup behind`() =
        runTest {
            val root = folder.newFolder("root")
            val unit = File(root, "slot-a").also { it.mkdirs() }
            repeat(3) { index ->
                File(unit, "part$index.bin").writeBytes("avant-$index".toByteArray())
            }

            val vfs = JvmVfs(mapOf("folder:0" to root.toPath()))
            val ref = RootRef("folder:0")

            repeat(3) { index ->
                val content = "apres-$index".toByteArray()
                vfs.writeAtomic(
                    ref,
                    "slot-a/part$index.bin",
                    ByteArrayInputStream(content),
                    sha256(content),
                )
            }

            // Trois écritures, zéro backup : c'est le moteur qui décide d'en
            // prendre un, et une seule fois pour l'unité entière.
            val backups = unit.listFiles()?.filter { ".rsc-bak" in it.name } ?: emptyList()
            assertEquals("writeAtomic ne doit prendre aucun backup", emptyList<File>(), backups)
        }

    @Test
    fun `an explicit backup produces exactly one set and rotates it`() =
        runTest {
            val root = folder.newFolder("root2")
            val unit = File(root, "slot-b").also { it.mkdirs() }
            File(unit, "save.bin").writeBytes("v1".toByteArray())

            val vfs = JvmVfs(mapOf("folder:0" to root.toPath()))
            val ref = RootRef("folder:0")

            vfs.backup(ref, "slot-b")
            assertEquals(1, root.listFiles()?.count { it.name == "slot-b.rsc-bak" })

            // Un second cycle fait tourner les générations, il n'en crée pas
            // une par fichier : la rotation reste au niveau de l'unité.
            vfs.backup(ref, "slot-b")
            val generations =
                root.listFiles()?.filter { it.name.startsWith("slot-b.rsc-bak") }?.map { it.name }
                    ?: emptyList()
            assertEquals(
                listOf("slot-b.rsc-bak", "slot-b.rsc-bak.1"),
                generations.sorted(),
            )
        }
}
