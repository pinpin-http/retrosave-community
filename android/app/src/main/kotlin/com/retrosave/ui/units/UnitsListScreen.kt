package com.retrosave.ui.units

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.retrosave.R
import com.retrosave.data.db.entities.UnitEntity
import com.retrosave.sync.SyncUiState
import com.retrosave.ui.theme.AccentCard
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.CardTitle
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.Reveal
import com.retrosave.ui.theme.ScreenTitle
import com.retrosave.ui.theme.SectionHeading
import com.retrosave.ui.theme.StatTile
import com.retrosave.ui.theme.StatusChip

/**
 * L'écran principal, en trois temps : le titre, LA carte d'état, puis la
 * bibliothèque.
 *
 * La carte verte est la seule surface pleine de l'écran — elle porte l'état de
 * la synchronisation et son bouton. Tout le reste est en cartes blanches. La
 * hiérarchie tient à la MATIÈRE, pas à la taille des titres : deux surfaces
 * pleines, et l'œil ne sait plus où est l'action attendue.
 */
@Composable
fun UnitsListScreen(
    units: List<UnitEntity>,
    syncState: SyncUiState,
    onSync: () -> Unit,
    onResume: () -> Unit,
    onAdapters: () -> Unit,
    onConnection: () -> Unit,
    onUnit: (Long) -> Unit,
    onConflicts: () -> Unit,
    // M8 §8 : fourni paresseusement, une unité à la fois. Lire toutes les
    // icônes avant le premier rendu figerait une bibliothèque de 300 unités.
    loadIcon: suspend (UnitEntity) -> ByteArray? = { null },
    // AD-36 (Q32) : ce qui est affiché est daté, et un déclencheur muet doit
    // devenir visible sans que l'utilisateur ait à le soupçonner.
    lastSuccessAtMs: Long = 0L,
    lastPeriodicSuccessAtMs: Long = 0L,
) {
    val running = syncState is SyncUiState.Running
    val report = (syncState as? SyncUiState.Completed)?.report
    val grouped = units.groupBy(UnitEntity::emulator).toSortedMap()

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Reveal(index = 0) {
                ScreenTitle(
                    bold = stringResource(R.string.units_title),
                )
            }
        }

        // — LA carte pleine : l'état de la synchronisation ——————————
        item {
            Reveal(index = 1) {
                AccentCard(working = running) {
                    Text(
                        text = stringResource(R.string.units_sync),
                        style = MaterialTheme.typography.titleLarge,
                        color = RetroSaveColors.onSecondaryFixed,
                    )
                    Text(
                        text =
                            when (syncState) {
                                is SyncUiState.Running -> stringResource(R.string.units_sync_running)
                                is SyncUiState.Failed -> syncState.message
                                else -> stringResource(R.string.units_close_emulator)
                            },
                        style = MaterialTheme.typography.bodyLarge,
                        // Pas d'opacité sur le vert : à 80 %, cette encre tombe
                        // à 3,8 de contraste. La hiérarchie passe par la taille.
                        color = RetroSaveColors.onSecondaryFixed,
                    )
                    // Les chiffres se lisent en TUILES, jamais en phrase : quatre
                    // tuiles disent en un regard ce qu'une ligne de rapport
                    // demande de déchiffrer. Et ils MONTENT jusqu'à leur valeur,
                    // ce qui fait remarquer qu'une passe vient de se terminer.
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        StatTile(
                            value = report?.pushed ?: 0,
                            caption = stringResource(R.string.units_stat_pushed),
                            modifier = Modifier.weight(1f),
                        )
                        StatTile(
                            value = report?.pulled ?: 0,
                            caption = stringResource(R.string.units_stat_pulled),
                            modifier = Modifier.weight(1f),
                        )
                        StatTile(
                            value = report?.conflicts ?: 0,
                            caption = stringResource(R.string.units_stat_conflicts),
                            modifier = Modifier.weight(1f),
                        )
                        StatTile(
                            value = report?.errors ?: 0,
                            caption = stringResource(R.string.units_stat_errors),
                            modifier = Modifier.weight(1f),
                        )
                    }
                    PillButton(
                        text = stringResource(R.string.units_sync),
                        onClick = onSync,
                        enabled = !running,
                        // Lancer une passe ENGAGE quelque chose : c'est un des
                        // rares gestes qui méritent une vibration. Une vibration
                        // sur chaque bouton finit par ne plus rien signifier.
                        haptic = true,
                    )
                }
            }
        }

        item {
            Reveal(index = 2) {
                FreshnessBanner(
                    lastSuccessAtMs = lastSuccessAtMs,
                    lastPeriodicSuccessAtMs = lastPeriodicSuccessAtMs,
                    nowMs = System.currentTimeMillis(),
                )
            }
        }

        // — Les bandeaux ————————————————————————————————
        val unconfigured = report?.unconfiguredAdapters?.sorted().orEmpty()
        if (unconfigured.isNotEmpty()) {
            item {
                // §9.6 : « non configuré », pas « à placer ». Le remède est un
                // dossier à choisir, pas un jeu à lancer.
                NoticeCard(
                    message =
                        stringResource(
                            R.string.units_unconfigured,
                            unconfigured.joinToString(", ") { emulatorLabel(it) },
                        ),
                    onClick = onAdapters,
                )
            }
        }
        val awaiting = report?.awaitingPlacement.orEmpty()
        if (awaiting.isNotEmpty()) {
            item {
                // Q29 : « à placer » — cible incalculable au sens d'AD-26. Sans
                // ce bandeau, l'unité restait simplement absente, sans que rien
                // n'explique pourquoi ni quoi faire.
                NoticeCard(
                    message = stringResource(R.string.units_awaiting_placement, awaiting.size),
                )
            }
        }
        val lostAdapters = report?.inaccessibleAdapters?.sorted().orEmpty()
        if (lostAdapters.isNotEmpty()) {
            item {
                // Q20 : le texte se construit ici, à partir de l'état typé. Le
                // bandeau *est* l'écran « accès perdu » du §14 : il mène aux
                // Adaptateurs, qui montrent l'état des trois racines.
                NoticeCard(
                    message =
                        if (lostAdapters.size == 1) {
                            stringResource(R.string.units_root_lost_one, lostAdapters.single())
                        } else {
                            stringResource(R.string.units_root_lost_many, lostAdapters.size)
                        },
                    bad = true,
                    onClick = onAdapters,
                )
            }
        }
        if (units.any { it.openConflictId != null }) {
            item {
                NoticeCard(
                    message = stringResource(R.string.units_conflict_banner),
                    bad = true,
                    onClick = onConflicts,
                )
            }
        }
        if (units.any { it.state == "paused" }) {
            item {
                PaperCard {
                    CardBody(
                        text = stringResource(R.string.units_paused_banner),
                        tone = RetroSaveColors.error,
                    )
                    PillButton(
                        text = stringResource(R.string.units_resume),
                        onClick = onResume,
                        quiet = true,
                    )
                }
            }
        }
        if (report != null && report.messages.isNotEmpty()) {
            item {
                NoticeCard(
                    message = report.messages.take(3).joinToString("\n"),
                    bad = report.errors > 0,
                )
            }
        }

        // — La bibliothèque ————————————————————————————
        if (units.isEmpty()) {
            item {
                PaperCard {
                    CardTitle(stringResource(R.string.units_empty_title))
                    PillButton(
                        text = stringResource(R.string.units_configure_adapters),
                        onClick = onAdapters,
                        quiet = true,
                    )
                }
            }
        } else {
            grouped.forEach { (emulator, emulatorUnits) ->
                item(key = "header-$emulator") {
                    SectionHeading(emulatorName(emulator))
                }
                items(emulatorUnits, key = { it.localId }) { unit ->
                    UnitCard(
                        unit = unit,
                        onClick = { onUnit(unit.localId) },
                        loadIcon = loadIcon,
                        // Q45 : une jaquette peut ARRIVER après coup — elle est
                        // téléchargée à la fin d'une passe. Sans une clé qui
                        // bouge, la vignette resterait sur sa pastille de repli
                        // jusqu'à la prochaine ouverture de l'application.
                        iconEpoch = lastSuccessAtMs,
                        // Une liste paresseuse n'utilise PAS `Reveal` : un élément
                        // recomposé en revenant en arrière rejouerait son entrée
                        // à chaque passage. `animateItem` n'anime que les vrais
                        // changements — une unité qui apparaît ou se déplace.
                        modifier = Modifier.animateItem(),
                    )
                }
            }
            item {
                PillButton(
                    text = stringResource(R.string.units_configure_adapters),
                    onClick = onAdapters,
                    modifier = Modifier.fillMaxWidth(),
                    quiet = true,
                    quietInk = RetroSaveColors.onSurface,
                )
            }
        }
        item {
            PillButton(
                text = stringResource(R.string.units_modify_connection),
                onClick = onConnection,
                modifier = Modifier.fillMaxWidth(),
                quiet = true,
                quietInk = RetroSaveColors.onSurface,
            )
        }
    }
}

/**
 * Un message, sur une carte blanche. Le rouge employé est celui « sur carte » :
 * le rouge du fond y serait illisible.
 */
@Composable
private fun NoticeCard(
    message: String,
    bad: Boolean = false,
    onClick: (() -> Unit)? = null,
) {
    PaperCard(onClick = onClick) {
        CardBody(
            text = message,
            tone = if (bad) RetroSaveColors.error else RetroSaveColors.onSurface,
        )
    }
}

@Composable
private fun UnitCard(
    unit: UnitEntity,
    onClick: () -> Unit,
    loadIcon: suspend (UnitEntity) -> ByteArray?,
    modifier: Modifier = Modifier,
    iconEpoch: Long = 0L,
) {
    var icon by remember(unit.localId) { mutableStateOf<ByteArray?>(null) }
    LaunchedEffect(unit.localId, unit.relPath, iconEpoch) {
        icon = runCatching { loadIcon(unit) }.getOrNull()
    }
    PaperCard(modifier = modifier, onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            GameThumbnail(
                gameKey = unit.gameKey,
                label = unit.gameLabel,
                icon = icon,
                modifier = Modifier.padding(end = 12.dp),
                emulator = unit.emulator,
            )
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = unit.gameLabel,
                    style = MaterialTheme.typography.titleMedium,
                    color = RetroSaveColors.onSurface,
                )
                Text(
                    text = unit.unitKey,
                    style = MaterialTheme.typography.bodySmall,
                    color = RetroSaveColors.onSurfaceVariant,
                )
            }
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    text = stringResource(R.string.unit_version, unit.lastSyncedVersion),
                    style = MaterialTheme.typography.labelLarge,
                    color = RetroSaveColors.onSurface,
                )
                StatusChip(
                    label = unitBadge(unit),
                    tint = badgeColor(unit),
                    paper = true,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun unitBadge(unit: UnitEntity): String =
    stringResource(
        when {
            unit.openConflictId != null -> R.string.unit_state_conflict
            unit.state == "paused" -> R.string.unit_state_paused
            unit.state == "missing" -> R.string.unit_state_missing
            unit.state == "unsupported" -> R.string.unit_state_unsupported
            // §9.6 : deux fichiers portent le même nom normalisé. Le moteur le
            // sait et refuse de pousser ; sans badge, l'utilisateur ne voyait
            // qu'une unité qui ne se synchronise jamais, sans raison affichée.
            unit.state == "duplicate" -> R.string.unit_state_duplicate
            unit.state == "error" -> R.string.unit_state_error
            unit.serverId == null || unit.lastSyncedVersion == 0 -> R.string.unit_state_push
            else -> R.string.unit_state_current
        },
    )

/**
 * La couleur du badge — sur une CARTE BLANCHE, donc jamais celle du fond.
 *
 * Le vert ne dit ici qu'une chose : à jour, en sûreté. Il ne signale jamais un
 * conflit ni une attente, et le mot du badge dit toujours la même chose que la
 * couleur (EXP-06).
 */
@Composable
private fun badgeColor(unit: UnitEntity): Color =
    when {
        unit.openConflictId != null || unit.state == "error" -> RetroSaveColors.error
        unit.state == "paused" || unit.state == "missing" || unit.state == "duplicate" ->
            RetroSaveColors.onSurfaceVariant
        unit.serverId == null || unit.lastSyncedVersion == 0 -> RetroSaveColors.onSurfaceVariant
        else -> RetroSaveColors.secondary
    }

@Composable
private fun emulatorName(emulator: String): String =
    when (emulator) {
        "ppsspp" -> "PPSSPP"
        "melonds" -> "melonDS"
        "azahar" -> "Azahar"
        else -> emulator
    }
