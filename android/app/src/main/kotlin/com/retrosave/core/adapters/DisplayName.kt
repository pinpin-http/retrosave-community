package com.retrosave.core.adapters

// Libellé présentable dérivé d'un nom de fichier — M8 §5.
// Miroir exact de `display_name.py`.
//
// **Distincte de `normalizeGameName`, volontairement.** Celle-là produit la clé
// d'appariement des unités : la toucher renommerait des `game_key` existants et
// casserait l'appariement entre appareils. Celle-ci ne sert qu'à l'affichage, et
// conserve la casse.
//
// Un seul principe : n'enlever que ce qu'on **reconnaît**. Retirer tout groupe
// entre parenthèses amputerait « Tony Hawk's Underground 2 (Remix) » ou
// « Zelda [Master Quest] », où le groupe fait partie du titre. Mieux vaut
// laisser un tag de trop que manger un mot du nom.

private val GROUP = Regex("""\(([^()]*)\)|\[([^\[\]]*)]""")
private val WHITESPACE = Regex("""\s+""")
private val EXTENSION = Regex("""^[A-Za-z0-9]{1,5}$""")

/** Régions et zones des jeux de nommage courants (No-Intro, GoodTools, TOSEC). */
private val REGIONS =
    setOf(
        "usa",
        "europe",
        "japan",
        "world",
        "france",
        "germany",
        "spain",
        "italy",
        "australia",
        "canada",
        "korea",
        "china",
        "taiwan",
        "brazil",
        "netherlands",
        "sweden",
        "norway",
        "denmark",
        "finland",
        "russia",
        "poland",
        "asia",
        "uk",
        "usa/europe",
        "japan/usa",
        "ntsc",
        "pal",
        "ntsc-u",
        "ntsc-j",
        "en",
        "fr",
        "de",
        "es",
        "it",
        "ja",
        "jp",
        "nl",
        "pt",
        "sv",
        "no",
        "da",
        "fi",
        "zh",
        "ko",
        "pl",
        "ru",
    )

/** Mentions d'édition qui ne font pas partie du titre. */
private val EDITIONS = setOf("proto", "beta", "demo", "sample", "unl", "alt", "kiosk", "prototype")

/** Drapeaux de dump GoodTools : une lettre, éventuellement suivie d'un numéro. */
private val DUMP_FLAG = Regex("""^[!abcfhopstux][0-9]*$""", RegexOption.IGNORE_CASE)

/** Traductions : `T+Fre`, `T-Eng`, `T+Ita1.0`. */
private val TRANSLATION = Regex("""^t[+-][a-z]{2,4}[0-9.]*$""", RegexOption.IGNORE_CASE)

/** Révisions et versions : `Rev 1`, `Rev A`, `v1.1`. */
private val REVISION = Regex("""^(rev\s*[0-9a-z]+|v[0-9][0-9a-z.]*)$""", RegexOption.IGNORE_CASE)

/** Rendre un libellé lisible, ou le nom brut si le nettoyage ne laisse rien. */
fun displayName(filename: String): String {
    val stem = stripExtension(filename)
    val withoutTags =
        GROUP.replace(stem) { match -> if (isKnownTag(match)) " " else match.value }
    val cleaned =
        WHITESPACE
            .replace(withoutTags.replace('_', ' ').replace('.', ' '), " ")
            .trim()
    if (cleaned.isNotEmpty()) return cleaned
    // Un nom entièrement composé de tags — ça existe — ne doit pas devenir une
    // ligne vide dans la bibliothèque : mieux vaut afficher le nom brut.
    return WHITESPACE.replace(stem, " ").trim()
}

private fun stripExtension(filename: String): String {
    val index = filename.lastIndexOf('.')
    // `index > 0` et pas `>= 0` : un fichier comme `.nomedia` n'a pas
    // d'extension, il a un nom qui commence par un point.
    if (index <= 0) return filename
    if (!EXTENSION.matches(filename.substring(index + 1))) return filename
    return filename.substring(0, index)
}

private fun isKnownTag(match: MatchResult): Boolean {
    val inside = match.groupValues[1].ifEmpty { match.groupValues[2] }
    val parts = inside.split(",").map { it.trim() }.filter { it.isNotEmpty() }
    if (parts.isEmpty()) return false
    // Tout le groupe doit être reconnu : « (Europe, Remix) » garde son sens et
    // reste affiché, plutôt que de perdre à moitié une information utile.
    return parts.all(::isKnownPart)
}

private fun isKnownPart(part: String): Boolean {
    val lowered = part.lowercase()
    if (lowered in REGIONS || lowered in EDITIONS) return true
    return DUMP_FLAG.matches(part) || TRANSLATION.matches(part) || REVISION.matches(part)
}
