package com.retrosave.core.fingerprint

import com.retrosave.core.model.NodeMeta
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.util.Arrays

/** Comparateur unique des chemins canoniques : octets UTF-8 non signés. */
val utf8PathComparator =
    Comparator<String> { left, right ->
        Arrays.compareUnsigned(
            left.toByteArray(StandardCharsets.UTF_8),
            right.toByteArray(StandardCharsets.UTF_8),
        )
    }

/** Sérialise exactement la grammaire QF figée par la décision Q8. */
fun canonicalQuickFingerprint(nodes: List<NodeMeta>): ByteArray {
    val canonical = ByteArrayOutputStream()
    nodes
        .sortedWith { left, right ->
            utf8PathComparator.compare(left.relPath, right.relPath)
        }.forEach { node ->
            canonical.write(node.relPath.toByteArray(StandardCharsets.UTF_8))
            canonical.write(0)
            canonical.write(node.sizeBytes.toString().toByteArray(StandardCharsets.US_ASCII))
            canonical.write(0)
            canonical.write((node.mtimeMs ?: 0).toString().toByteArray(StandardCharsets.US_ASCII))
            canonical.write('\n'.code)
        }
    return canonical.toByteArray()
}

/** Retourne le SHA-256 hexadécimal minuscule de l'empreinte canonique. */
fun quickFingerprintHash(nodes: List<NodeMeta>): String =
    MessageDigest
        .getInstance("SHA-256")
        .digest(canonicalQuickFingerprint(nodes))
        .joinToString("") { byte ->
            "%02x".format(byte.toInt() and 0xff)
        }
