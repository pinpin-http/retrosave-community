package com.retrosave.core.adapters

private const val ROM_EXTENSION = ".nds"
private const val SAVE_EXTENSION = ".sav"

/**
 * Directory of the uniquely matching ROM, or null to leave the unit unplaced (AD-26).
 *
 * Matching is on the file name alone — a ROM is never opened, read or hashed (I1).
 */
fun melonDsRomDirectory(
    unitKey: String,
    romRelPaths: List<String>,
): String? {
    if (!unitKey.lowercase().endsWith(SAVE_EXTENSION)) return null
    val stem = unitKey.dropLast(SAVE_EXTENSION.length)
    val matches =
        romRelPaths.distinct().filter { relPath ->
            matchesRomName(relPath.substringAfterLast('/'), stem)
        }
    if (matches.size != 1) return null
    val match = matches.single()
    return if (match.contains('/')) match.substringBeforeLast('/') else ""
}

private fun matchesRomName(
    fileName: String,
    stem: String,
): Boolean {
    if (!fileName.lowercase().endsWith(ROM_EXTENSION)) return false
    return fileName.dropLast(ROM_EXTENSION.length) == stem
}
