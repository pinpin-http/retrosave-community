package com.retrosave.core.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Reproduit trois défauts constatés sur le Thor le 02/08, avant correctif.
 *
 * Les trois venaient de la même erreur : apparier des événements *à l'intérieur*
 * d'une fenêtre au lieu de lire l'état courant d'un package. Écrits d'après des
 * traces réelles de `dumpsys usagestats`, pas d'après ce que le code faisait.
 */
class UsageFoldTest {
    private val ppsspp = "org.ppsspp.ppsspp"
    private val melonds = "me.magnum.melondualds"

    @Test
    fun `a game running since before the window is still running`() {
        // Trace réelle : RESUMED a 22:40:27, plus aucun evenement ensuite, et le
        // process vivant a 22:44:46. Le service sondait sur deux minutes : sa
        // fenetre ne contenait rien, et il en concluait que la partie etait
        // finie — donc il synchronisait pendant que le jeu tournait, puis
        // s'arretait. L'etat connu doit survivre a une fenetre vide.
        val result = foldUsageEvents(known = setOf(ppsspp), events = emptyList(), sinceMs = 0)

        assertEquals(setOf(ppsspp), result.running)
        assertTrue("une fenetre vide ne termine aucune session", result.ended.isEmpty())
    }

    @Test
    fun `a session ending after the cursor is reported even without its start`() {
        // Une partie plus longue qu'un cycle WorkManager : le RESUMED a ete
        // consomme par une fenetre precedente, seul le PAUSED tombe ici. C'est
        // le cas nominal d'une vraie soiree, et il ne declenchait rien.
        val result =
            foldUsageEvents(
                known = setOf(ppsspp),
                events = listOf(UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_PAUSED, 5_000)),
                sinceMs = 1_000,
            )

        assertEquals(setOf(ppsspp), result.ended)
        assertTrue(result.running.isEmpty())
    }

    @Test
    fun `a game killed rather than paused still ends its session`() {
        // Trace reelle : un `am force-stop` produit ACTIVITY_STOPPED et jamais
        // ACTIVITY_PAUSED. Ignorer STOPPED laissait la partie « en cours » pour
        // toujours — et l'utilisateur qui tue son jeu depuis les recents est un
        // cas ordinaire sur une console portable.
        val result =
            foldUsageEvents(
                known = setOf(ppsspp),
                events = listOf(UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_STOPPED, 5_000)),
                sinceMs = 1_000,
            )

        assertEquals(setOf(ppsspp), result.ended)
        assertTrue(result.running.isEmpty())
    }

    @Test
    fun `a session that ended before the cursor is not replayed`() {
        val result =
            foldUsageEvents(
                known = emptySet(),
                events =
                    listOf(
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 1_000),
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_PAUSED, 2_000),
                    ),
                sinceMs = 9_000,
            )

        assertTrue("sinon chaque passe rejouerait la meme partie", result.ended.isEmpty())
    }

    @Test
    fun `a relaunch after a pause cancels the end`() {
        // Changer de jeu, ou revenir apres un aller-retour dans le launcher : la
        // partie n'est pas finie, et une passe lancee ici capturerait un fichier
        // que l'emulateur tient ouvert.
        val result =
            foldUsageEvents(
                known = emptySet(),
                events =
                    listOf(
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 1_000),
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_PAUSED, 2_000),
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 3_000),
                    ),
                sinceMs = 0,
            )

        assertEquals(setOf(ppsspp), result.running)
        assertTrue(result.ended.isEmpty())
    }

    @Test
    fun `events out of order do not invert a package state`() {
        val result =
            foldUsageEvents(
                known = emptySet(),
                events =
                    listOf(
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_PAUSED, 9_000),
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 1_000),
                    ),
                sinceMs = 0,
            )

        assertEquals(setOf(ppsspp), result.ended)
        assertTrue(result.running.isEmpty())
    }

    @Test
    fun `a persisted running state that a deep window cannot corroborate is stale`() {
        // Exigence de l'arbitrage Q31 : l'état de session est persisté, donc il
        // survit à la mort du process — et il peut lui survivre à tort. Si même
        // une fenêtre profonde ne retrouve aucun événement du package, le croire
        // indéfiniment condamnerait l'unité à ne plus jamais être poussée après
        // une partie. Un état bloqué est pire qu'un déclenchement de trop.
        val result =
            foldUsageEvents(
                known = setOf(ppsspp),
                events = emptyList(),
                sinceMs = 1_000,
                nowMs = 50_000,
                corroborating = true,
            )

        assertEquals(setOf(ppsspp), result.ended)
        assertTrue(result.running.isEmpty())
    }

    @Test
    fun `a shallow window never invents the end of a session`() {
        // Le pendant du cas précédent, et la raison pour laquelle le drapeau
        // existe : sur une fenêtre courte, l'absence d'événement ne prouve rien.
        // C'est exactement l'erreur mesurée sur le Thor le 02/08.
        val result =
            foldUsageEvents(
                known = setOf(ppsspp),
                events = emptyList(),
                sinceMs = 1_000,
                nowMs = 50_000,
                corroborating = false,
            )

        assertEquals(setOf(ppsspp), result.running)
        assertTrue(result.ended.isEmpty())
    }

    @Test
    fun `a corroborated running game stays running`() {
        val result =
            foldUsageEvents(
                known = setOf(ppsspp),
                events = listOf(UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 2_000)),
                sinceMs = 1_000,
                nowMs = 50_000,
                corroborating = true,
            )

        assertEquals(setOf(ppsspp), result.running)
        assertTrue(result.ended.isEmpty())
    }

    @Test
    fun `each adapter keeps its own state`() {
        val result =
            foldUsageEvents(
                known = setOf(melonds),
                events =
                    listOf(
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_RESUMED, 1_000),
                        UsageEventRecord(ppsspp, UsageEventType.ACTIVITY_PAUSED, 2_000),
                    ),
                sinceMs = 0,
            )

        assertEquals(setOf(melonds), result.running)
        assertEquals(setOf(ppsspp), result.ended)
    }
}
