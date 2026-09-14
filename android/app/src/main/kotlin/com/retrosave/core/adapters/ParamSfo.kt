package com.retrosave.core.adapters

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

// Parseur `PARAM.SFO` (format PSF) — M8 §4. Miroir exact de `param_sfo.py`.
//
// Un dossier `PSP/SAVEDATA/<unit>/` contient un `PARAM.SFO` : c'est un fichier
// de **sauvegarde**, pas une ROM, donc le lire ne touche pas I1.
//
// Deux règles gouvernent tout ce fichier :
//
// - **Aucune exception ne remonte.** Ce sont des fichiers du monde réel, écrits
//   par des jeux de 2005 et recopiés par des outils approximatifs. Un SFO
//   illisible coûte un libellé moins joli, jamais une passe (AD-28, M8 §10).
// - **Le résultat est identique en Python et en Kotlin.** Deux implémentations
//   divergentes donneraient deux titres différents pour le même fichier selon
//   l'appareil qui découvre l'unité en premier — d'où les vecteurs partagés.

private const val HEADER_SIZE = 20
private const val INDEX_ENTRY_SIZE = 16

/** Format de données UTF-8 terminé par un octet nul ; aucun titre n'a d'autre type. */
private const val FMT_UTF8 = 0x0204

/** §4 : un `PARAM.SFO` fait quelques centaines d'octets. Au-delà, c'est autre chose. */
const val MAX_SFO_BYTES: Int = 64 * 1024

private const val TITLE_KEY = "TITLE"
private const val SAVEDATA_TITLE_KEY = "SAVEDATA_TITLE"
private val MAGIC = byteArrayOf(0x00, 'P'.code.toByte(), 'S'.code.toByte(), 'F'.code.toByte())

/**
 * Ce qu'on retient d'un `PARAM.SFO`.
 *
 * [title] alimente `game_label` ; [savedataTitle] est le nom du slot
 * (« Chapitre 3 ») et servira au libellé de slot plus tard. Les confondre
 * afficherait « Chapitre 3 » comme nom de jeu.
 */
data class ParamSfo(
    val title: String?,
    val savedataTitle: String?,
)

/**
 * Rendre les titres d'un `PARAM.SFO`, ou `null` si ce n'en est pas un.
 *
 * `null` signifie « ce fichier n'est pas un SFO exploitable » : magic absent,
 * en-tête tronqué, taille aberrante. Un SFO valide mais dont les champs sont
 * absents ou illisibles rend un [ParamSfo] aux champs nuls — la distinction
 * sépare « pas un SFO » de « un SFO sans titre ».
 */
fun parseParamSfo(data: ByteArray): ParamSfo? {
    if (data.size > MAX_SFO_BYTES || data.size < HEADER_SIZE) return null
    if (!data.startsWith(MAGIC)) return null

    val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    buffer.position(4)
    @Suppress("UNUSED_VARIABLE")
    val version = buffer.int
    val keyTable = buffer.int.toUInt().toLong()
    val dataTable = buffer.int.toUInt().toLong()
    val entryCount = buffer.int.toUInt().toLong()

    // Un `entryCount` qui déborde du fichier est le symptôme le plus courant d'un
    // SFO tronqué : aucune borne d'entrée n'est alors fiable, et lire celles qui
    // « tiennent » reviendrait à interpréter des octets pris ailleurs.
    val indexEnd = HEADER_SIZE + entryCount * INDEX_ENTRY_SIZE
    if (indexEnd > data.size) return null
    if (keyTable > data.size || dataTable > data.size) return ParamSfo(null, null)

    var title: String? = null
    var savedataTitle: String? = null

    for (position in 0 until entryCount.toInt()) {
        val offset = HEADER_SIZE + position * INDEX_ENTRY_SIZE
        buffer.position(offset)
        val keyOffset = buffer.short.toInt() and 0xFFFF
        val fmt = buffer.short.toInt() and 0xFFFF
        val valueLen = buffer.int.toUInt().toLong()

        @Suppress("UNUSED_VARIABLE")
        val maxLen = buffer.int
        val valueOffset = buffer.int.toUInt().toLong()

        val key = readKey(data, keyTable + keyOffset) ?: continue
        if (key != TITLE_KEY && key != SAVEDATA_TITLE_KEY) continue
        // Un `TITLE` déclaré entier est refusé plutôt que réinterprété : le
        // fichier se contredit, et deviner produirait un titre inventé.
        if (fmt != FMT_UTF8) continue

        val value = readUtf8(data, dataTable + valueOffset, valueLen) ?: continue
        if (key == TITLE_KEY) title = value else savedataTitle = value
    }

    return ParamSfo(title = title, savedataTitle = savedataTitle)
}

private fun ByteArray.startsWith(prefix: ByteArray): Boolean {
    if (size < prefix.size) return false
    return prefix.indices.all { this[it] == prefix[it] }
}

private fun readKey(
    data: ByteArray,
    start: Long,
): String? {
    if (start < 0 || start >= data.size) return null
    var end = start.toInt()
    while (end < data.size && data[end] != 0.toByte()) end++
    if (end >= data.size) return null
    return decodeUtf8(data, start.toInt(), end - start.toInt())
}

private fun readUtf8(
    data: ByteArray,
    start: Long,
    length: Long,
): String? {
    if (start < 0 || length < 0 || start + length > data.size) return null
    // Le format déclare une longueur qui inclut le nul terminal, et certains
    // jeux en alignent plusieurs. Couper au premier évite un titre suivi de
    // caractères invisibles qui casseraient la comparaison avec l'autre client.
    var end = start.toInt()
    val limit = (start + length).toInt()
    while (end < limit && data[end] != 0.toByte()) end++
    val size = end - start.toInt()
    if (size <= 0) return null
    return decodeUtf8(data, start.toInt(), size)
}

/**
 * Décoder strictement : un octet invalide rend `null`, jamais un caractère de
 * remplacement.
 *
 * Le défaut de Kotlin remplacerait silencieusement par « ￿ », et les deux
 * clients afficheraient alors un titre différemment cabossé pour le même
 * fichier — exactement ce que les vecteurs partagés existent pour empêcher.
 */
private fun decodeUtf8(
    data: ByteArray,
    offset: Int,
    length: Int,
): String? {
    val decoder =
        StandardCharsets.UTF_8
            .newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
    return try {
        decoder.decode(ByteBuffer.wrap(data, offset, length)).toString()
    } catch (_: java.nio.charset.CharacterCodingException) {
        null
    }
}
