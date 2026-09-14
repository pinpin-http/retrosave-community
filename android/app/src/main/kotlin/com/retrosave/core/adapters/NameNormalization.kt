package com.retrosave.core.adapters

private val tagPattern = Regex("""\([^)]*\)|\[[^\]]*]""")
private val whitespacePattern = Regex("""\s+""")

/**
 * Produit la clé de jeu commune à melonDS et, après le POC, RetroArch.
 *
 * L'extension finale et tous les groupes `()` / `[]` sont retirés avant la
 * mise en minuscules, le remplacement des underscores et le compactage des
 * espaces.
 */
fun normalizeGameName(filename: String): String {
    val withoutExtension = filename.substringBeforeLast('.', filename)
    val withoutTags = tagPattern.replace(withoutExtension, " ")
    return whitespacePattern
        .replace(withoutTags.replace('_', ' ').lowercase(), " ")
        .trim()
}
