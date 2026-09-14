package com.retrosave.ui.units

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.retrosave.R
import com.retrosave.core.sync.RemoteVersion
import com.retrosave.data.db.entities.UnitEntity
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.CardTitle
import com.retrosave.ui.theme.CircleButton
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.Reveal
import com.retrosave.ui.theme.ScreenTitle
import com.retrosave.ui.theme.StatusChip
import kotlinx.coroutines.launch

@Composable
fun UnitDetailScreen(
    unit: UnitEntity?,
    onBack: () -> Unit,
    loadVersions: suspend (String) -> Result<Pair<List<RemoteVersion>, Int>>,
    restore: suspend (String, Int) -> Result<Unit>,
    retry: suspend (Long) -> Unit,
) {
    val scope = rememberCoroutineScope()
    var versions by remember { mutableStateOf<List<RemoteVersion>>(emptyList()) }
    var headVersion by remember { mutableStateOf(0) }
    var loading by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var restoreCandidate by remember { mutableStateOf<RemoteVersion?>(null) }
    val historyError = stringResource(R.string.error_history_generic)
    val restoreError = stringResource(R.string.error_restore_generic)

    fun refresh(serverId: String) {
        scope.launch {
            loading = true
            error = null
            loadVersions(serverId)
                .onSuccess {
                    versions = it.first
                    headVersion = it.second
                }.onFailure {
                    error = it.message ?: historyError
                }
            loading = false
        }
    }

    LaunchedEffect(unit?.serverId) {
        val serverId = unit?.serverId
        if (serverId != null) refresh(serverId)
    }

    restoreCandidate?.let { version ->
        // La boîte de dialogue est BLANCHE, comme les cartes : `colorScheme.surface`
        // porte déjà cette couleur, elle n'a rien à redéclarer.
        AlertDialog(
            onDismissRequest = { if (!loading) restoreCandidate = null },
            title = {
                Text(stringResource(R.string.detail_restore_confirm_title, version.number))
            },
            text = { Text(stringResource(R.string.detail_restore_confirm_body)) },
            confirmButton = {
                PillButton(
                    text = stringResource(R.string.detail_restore_action),
                    enabled = !loading,
                    // Restaurer engage une écriture locale : l'un des rares
                    // gestes qui méritent une vibration.
                    haptic = true,
                    onClick = {
                        val serverId = unit?.serverId ?: return@PillButton
                        loading = true
                        error = null
                        scope.launch {
                            restore(serverId, version.number)
                                .onSuccess {
                                    restoreCandidate = null
                                    refresh(serverId)
                                }.onFailure {
                                    error = it.message ?: restoreError
                                    loading = false
                                }
                        }
                    },
                )
            },
            dismissButton = {
                PillButton(
                    text = stringResource(R.string.cancel),
                    enabled = !loading,
                    quiet = true,
                    onClick = { restoreCandidate = null },
                )
            },
        )
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Reveal(index = 0) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    // Le retour est un CERCLE, jamais un second rectangle : il ne
                    // doit pas rivaliser avec l'action de l'écran.
                    CircleButton(
                        glyph = "←",
                        onClick = onBack,
                        description = stringResource(R.string.back),
                        modifier = Modifier.padding(end = 12.dp),
                    )
                    ScreenTitle(
                        // `weight` et non `fillMaxWidth` : dans une ligne, un
                        // titre qui réclame toute la largeur pousse le cercle de
                        // retour hors de l'écran.
                        modifier = Modifier.weight(1f),
                        bold = unit?.gameLabel ?: stringResource(R.string.detail_unknown_unit),
                        subtitle =
                            unit?.let { "${emulatorLabel(it.emulator)} · ${it.unitKey}" },
                    )
                }
            }
        }
        // AD-30 : une unité mise en quarantaine doit pouvoir repartir depuis
        // l'appareil. Sans ce bouton, il fallait le PC et `rsc retry` — ce qui
        // rend la garde inutilisable là où elle se déclenche.
        if (unit != null && unit.state == "error") {
            item {
                Reveal(index = 1) {
                    PaperCard {
                        CardBody(
                            text = stringResource(R.string.detail_quarantined),
                            tone = RetroSaveColors.error,
                        )
                        PillButton(
                            text = stringResource(R.string.detail_retry),
                            enabled = !loading,
                            onClick = { scope.launch { retry(unit.localId) } },
                        )
                    }
                }
            }
        }
        if (loading) {
            item {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(20.dp),
                        color = RetroSaveColors.secondary,
                        strokeWidth = 2.dp,
                    )
                    Text(
                        text = stringResource(R.string.detail_loading),
                        style = MaterialTheme.typography.bodyLarge,
                        color = RetroSaveColors.outline,
                    )
                }
            }
        }
        error?.let { message ->
            item {
                PaperCard {
                    CardBody(text = message, tone = RetroSaveColors.error)
                }
            }
        }
        when {
            unit == null -> item { EmptyNote(stringResource(R.string.detail_unknown_unit)) }
            unit.serverId == null -> item { EmptyNote(stringResource(R.string.detail_not_uploaded)) }
            !loading && versions.isEmpty() ->
                item { EmptyNote(stringResource(R.string.detail_empty_history)) }
            else ->
                items(
                    items = versions.sortedByDescending(RemoteVersion::number),
                    key = RemoteVersion::number,
                ) { version ->
                    VersionCard(
                        version = version,
                        isHead = version.number == headVersion,
                        enabled = !loading && version.number != headVersion,
                        onRestore = { restoreCandidate = version },
                        modifier = Modifier.animateItem(),
                    )
                }
        }
    }
}

@Composable
private fun EmptyNote(text: String) {
    PaperCard { CardBody(text) }
}

@Composable
private fun VersionCard(
    version: RemoteVersion,
    isHead: Boolean,
    enabled: Boolean,
    onRestore: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PaperCard(modifier = modifier) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CardTitle(stringResource(R.string.detail_version_number, version.number))
            if (isHead) {
                // La tête actuelle est ce qui est EN SÛRETÉ : c'est le seul
                // endroit de la carte où le vert a sa place.
                StatusChip(
                    label = stringResource(R.string.detail_current_head),
                    tint = RetroSaveColors.secondary,
                    paper = true,
                )
            }
        }
        CardBody(
            stringResource(
                R.string.detail_version_metadata,
                version.createdAt,
                version.originDeviceName ?: stringResource(R.string.detail_unknown_device),
            ),
        )
        Text(
            text =
                stringResource(
                    R.string.detail_version_kind_size,
                    kindLabel(version.kind),
                    formatBytes(version.sizeBytes),
                ),
            style = MaterialTheme.typography.bodySmall,
            color = RetroSaveColors.onSurfaceVariant,
        )
        PillButton(
            text =
                stringResource(
                    if (isHead) R.string.detail_current_version else R.string.detail_restore_action,
                ),
            enabled = enabled,
            quiet = true,
            onClick = onRestore,
        )
    }
}

@Composable
private fun kindLabel(kind: String): String =
    stringResource(
        when (kind) {
            "restore" -> R.string.detail_kind_restore
            "conflict_branch" -> R.string.detail_kind_conflict
            else -> R.string.detail_kind_normal
        },
    )

internal fun emulatorLabel(emulator: String): String =
    when (emulator) {
        "ppsspp" -> "PPSSPP"
        "melonds" -> "melonDS"
        "azahar" -> "Azahar"
        "folder" -> "Generic folder"
        else -> emulator
    }

private fun formatBytes(bytes: Long): String =
    when {
        bytes >= 1024L * 1024L -> "%.1f Mio".format(bytes / (1024.0 * 1024.0))
        bytes >= 1024L -> "%.1f Kio".format(bytes / 1024.0)
        else -> "$bytes o"
    }
