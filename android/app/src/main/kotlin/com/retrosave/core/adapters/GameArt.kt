package com.retrosave.core.adapters

import java.security.MessageDigest

// Icônes et placeholders — M8 §8, partie pure. Miroir exact de `game_art.py`.
//
// Le placeholder doit être **identique sur les deux clients** pour le même jeu.
// Sinon la même sauvegarde change de couleur selon l'appareil, ce qui donne
// l'impression qu'il s'agit de deux jeux différents.
//
// La validation ne décode pas l'image : elle lit l'en-tête. C'est un filtre bon
// marché qui écarte ce qui n'est manifestement pas un PNG avant de le confier à
// un décodeur — un `.sav` renommé, un fichier tronqué, une image absurde.

val PNG_SIGNATURE = byteArrayOf(-119, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)

/** §4 : plafond de lecture d'une icône ; les vraies font quelques kilo-octets. */
const val MAX_ICON_BYTES: Int = 256 * 1024

/**
 * Plafond d'une jaquette **téléchargée** (Q45).
 *
 * Plus haut que [MAX_ICON_BYTES], et volontairement : une jaquette de
 * catalogue pèse quelques centaines de kilo-octets là où un `ICON0.PNG` en
 * pèse dix. Les deux bornes restent distinctes parce qu'elles protègent de
 * deux choses différentes — l'une lit le stockage de l'utilisateur, l'autre
 * borne un téléchargement que nous avons nous-mêmes déclenché.
 */
const val MAX_ARTWORK_BYTES: Int = 2 * 1024 * 1024

/** Ce qu'on refuse : les dimensions qui feraient exploser un décodeur. */
const val MAX_ICON_DIMENSION: Int = 4096

data class IconInfo(
    val width: Int,
    val height: Int,
)

/** Valider la signature et l'en-tête IHDR, ou rendre `null`. */
fun readPngHeader(
    data: ByteArray,
    maxBytes: Int = MAX_ICON_BYTES,
): IconInfo? {
    if (data.isEmpty() || data.size > maxBytes) return null
    if (data.size < 33) return null
    for (index in PNG_SIGNATURE.indices) {
        if (data[index] != PNG_SIGNATURE[index]) return null
    }
    // Longueur du premier chunk (13) puis son type : IHDR est obligatoirement le
    // premier, un fichier qui commence autrement n'est pas un PNG valide.
    if (readInt(data, 8) != 13) return null
    if (data[12] != 'I'.code.toByte() ||
        data[13] != 'H'.code.toByte() ||
        data[14] != 'D'.code.toByte() ||
        data[15] != 'R'.code.toByte()
    ) {
        return null
    }

    val width = readInt(data, 16)
    val height = readInt(data, 20)
    if (width <= 0 || height <= 0) return null
    if (width > MAX_ICON_DIMENSION || height > MAX_ICON_DIMENSION) return null
    return IconInfo(width = width, height = height)
}

// `maxBytes` a une valeur par défaut : les appelants existants — et les
// vecteurs partagés — gardent exactement le comportement d'avant.
fun isValidIcon(
    data: ByteArray,
    maxBytes: Int = MAX_ICON_BYTES,
): Boolean = readPngHeader(data, maxBytes) != null

/**
 * Teinte 0–359 dérivée du `gameKey`, stable et identique partout.
 *
 * Le `gameKey` plutôt que le libellé : le libellé change au renommage ou quand
 * un client apprend à lire le SFO, et une vignette qui change de couleur
 * donnerait l'impression d'un autre jeu.
 *
 * sha256 plutôt que `hashCode()` : celui de Kotlin n'est pas celui de Python, et
 * la couleur ne serait alors ni stable ni partagée.
 */
fun placeholderHue(gameKey: String): Int {
    val digest = MessageDigest.getInstance("SHA-256").digest(gameKey.toByteArray(Charsets.UTF_8))
    var value = 0L
    for (index in 0 until 4) {
        value = (value shl 8) or (digest[index].toLong() and 0xFF)
    }
    return (value % 360).toInt()
}

private val STOP_WORDS =
    setOf("the", "a", "an", "of", "le", "la", "les", "un", "une", "de", "du", "des")

/**
 * Une ou deux lettres tirées du libellé, en majuscules.
 *
 * Les mots vides sautent, mais jamais s'il ne reste rien : « The Legend of
 * Zelda » donne « LZ », alors que « The Sims » doit encore afficher quelque
 * chose.
 */
fun placeholderInitials(label: String): String {
    val words = tokenize(label)
    if (words.isEmpty()) return "?"
    val meaningful = words.filter { it.lowercase() !in STOP_WORDS }.ifEmpty { words }
    if (meaningful.size == 1) return meaningful[0].take(2).uppercase()
    return (meaningful[0].take(1) + meaningful[1].take(1)).uppercase()
}

private fun tokenize(label: String): List<String> {
    val words = mutableListOf<String>()
    val current = StringBuilder()
    for (character in label) {
        if (character.isLetterOrDigit()) {
            current.append(character)
        } else if (current.isNotEmpty()) {
            words += current.toString()
            current.setLength(0)
        }
    }
    if (current.isNotEmpty()) words += current.toString()
    return words
}

private fun readInt(
    data: ByteArray,
    offset: Int,
): Int {
    var value = 0
    for (index in 0 until 4) {
        value = (value shl 8) or (data[offset + index].toInt() and 0xFF)
    }
    return value
}
