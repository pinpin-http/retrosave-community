package com.retrosave.core.sync

const val APPLY_NO_JOURNAL = "no_journal"
const val APPLY_CLOSE = "close"
const val APPLY_REAPPLY = "reapply"

/**
 * État du marqueur d'application (AD-27, arbitrage Q19).
 *
 * Trois états, donc un type somme : deux chaînes nullables ne peuvent pas les
 * encoder sans sentinelle magique, et c'est précisément ce que Q16 interdit.
 */
sealed interface ApplyJournal {
    /** Aucun marqueur : l'application précédente, s'il y en a eu une, s'est close. */
    data object Absent : ApplyJournal

    /**
     * Un marqueur existe mais ne dit pas quel contenu attendre.
     *
     * Q17 a rendu cet état possible en théorie en stockant le marqueur sur
     * trois colonnes nullables. Il doit rester inatteignable en pratique — seule
     * la paire d'écrivains y touche — donc en rencontrer un mérite un WARN.
     */
    data object Malformed : ApplyJournal

    /** Marqueur complet, nommant le contenu que l'application écrivait. */
    data class Present(
        val expectedContentSha256: String,
    ) : ApplyJournal
}

/**
 * Transforme les trois colonnes stockées en la seule valeur que voit le moteur.
 *
 * Lecteur unique, pendant de l'écrivain unique : rien d'autre n'interprète ces
 * colonnes, donc une ligne partielle ne peut pas être lue de trois façons
 * différentes à trois endroits.
 */
fun readApplyJournal(
    version: Int?,
    contentSha256: String?,
    startedAt: Long?,
): ApplyJournal {
    val fields = listOf(version, contentSha256, startedAt)
    if (fields.all { it == null }) return ApplyJournal.Absent
    if (fields.any { it == null }) return ApplyJournal.Malformed
    return ApplyJournal.Present(requireNotNull(contentSha256))
}

/**
 * Décide ce qu'une application interrompue doit à la passe suivante.
 *
 * [localContentSha256] est le contenu réellement sur le disque, ou `null` quand
 * la cible est absente ou illisible — crash entre la suppression et l'écriture
 * du chemin de repli, ou unité-dossier amputée.
 *
 * Seule une égalité exacte clôt un marqueur sans retoucher la cible. Tout le
 * reste réapplique, ce qui n'est jamais destructif : le pull retélécharge la
 * tête, le `.rsc-bak` porte encore l'état d'avant le pull, et une unité sous
 * marqueur n'est jamais poussée.
 */
fun applyJournalDecision(
    journal: ApplyJournal,
    localContentSha256: String?,
): String =
    when (journal) {
        is ApplyJournal.Absent -> APPLY_NO_JOURNAL
        is ApplyJournal.Malformed -> APPLY_REAPPLY
        is ApplyJournal.Present ->
            if (localContentSha256 == journal.expectedContentSha256) {
                APPLY_CLOSE
            } else {
                APPLY_REAPPLY
            }
    }
