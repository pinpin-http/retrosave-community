package com.retrosave.data.session

import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import com.retrosave.R
import com.retrosave.RetroSaveApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Tuile Quick Settings « Sync RetroSave » (AD-34).
 *
 * Remplace l'étage 2 retiré (Q31). Un clic sur une tuile est une interaction
 * utilisateur avec un élément d'interface : le démarrage d'un service de premier
 * plan y est légal, sans permission supplémentaire et sans service résident.
 *
 * Elle est atteignable **depuis l'intérieur d'un jeu** — le volet se déroule
 * par-dessus le plein écran. Le geste que l'étage 2 tentait d'éviter revient à
 * deux glissements et une tape, avec zéro fragilité.
 */
class SyncTileService : TileService() {
    private var scope: CoroutineScope? = null

    override fun onStartListening() {
        super.onStartListening()
        refresh()
    }

    override fun onClick() {
        super.onClick()
        val app = application as RetroSaveApp
        val container = app.container
        val running = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        scope?.cancel()
        scope = running

        running.launch {
            val settings = container.settings.snapshot()
            if (!settings.isConnected) {
                show(Tile.STATE_INACTIVE, getString(R.string.tile_not_connected))
                return@launch
            }

            // Garde du cas 6 : rien ne s'écrit ni ne se capture pendant qu'un
            // émulateur est au premier plan. La tuile est justement le bouton
            // qu'on presse sans quitter son jeu — c'est donc ici, et pas
            // ailleurs, que la garde doit être visible plutôt que silencieuse.
            val now = System.currentTimeMillis()
            val window =
                container.usageSessions.read(
                    packages = container.watchedPackages(),
                    sinceMs = settings.lastUsageQueryMs,
                    nowMs = now,
                    known = settings.runningSessions,
                )
            if (window.running.isNotEmpty()) {
                show(Tile.STATE_INACTIVE, getString(R.string.tile_close_game_first))
                return@launch
            }

            show(Tile.STATE_ACTIVE, getString(R.string.tile_running))
            SyncPassService.start(applicationContext)
        }
    }

    override fun onStopListening() {
        scope?.cancel()
        scope = null
        super.onStopListening()
    }

    private fun refresh() {
        show(Tile.STATE_INACTIVE, getString(R.string.tile_idle))
    }

    private fun show(
        state: Int,
        subtitle: String,
    ) {
        val tile = qsTile ?: return
        tile.state = state
        tile.label = getString(R.string.tile_label)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            tile.subtitle = subtitle
        }
        tile.updateTile()
    }
}
