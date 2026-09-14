package com.retrosave.ui.conflict

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import com.retrosave.core.sync.ConflictHead
import com.retrosave.core.sync.RemoteConflict
import com.retrosave.core.sync.RemoteVersion
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.CardTitle
import com.retrosave.ui.theme.CircleButton
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.Reveal
import com.retrosave.ui.theme.ScreenTitle
import kotlinx.coroutines.launch

/**
 * Écran de résolution A/B imposé par §9.6.
 *
 * Le serveur ne renvoie que numéro, taille et empreinte pour chaque branche ;
 * l'appareil et la date viennent de l'historique de l'unité, sans endpoint
 * supplémentaire (§15).
 *
 * **Aucun vert ici.** Un conflit n'est pas une erreur, mais ce n'est pas non
 * plus « en sûreté » : les deux branches se présentent à égalité, en cartes
 * blanches, et rien ne suggère laquelle choisir. C'est à l'utilisateur de
 * trancher, pas à la couleur.
 */
@Composable
fun ConflictScreen(
    onBack: () -> Unit,
    loadConflicts: suspend () -> Result<List<RemoteConflict>>,
    loadVersions: suspend (String) -> Result<Pair<List<RemoteVersion>, Int>>,
    resolve: suspend (conflictId: String, winner: Int) -> Result<Unit>,
) {
    val scope = rememberCoroutineScope()
    var conflicts by remember { mutableStateOf<List<RemoteConflict>>(emptyList()) }
    var versionsByUnit by remember { mutableStateOf<Map<String, List<RemoteVersion>>>(emptyMap()) }
    var loading by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val genericError = stringResource(R.string.error_conflicts_generic)

    fun refresh() {
        scope.launch {
            loading = true
            error = null
            loadConflicts()
                .onSuccess { open ->
                    conflicts = open
                    versionsByUnit =
                        open
                            .associate { conflict ->
                                conflict.unitId to
                                    loadVersions(conflict.unitId)
                                        .map { it.first }
                                        .getOrDefault(emptyList())
                            }
                }.onFailure { error = it.message ?: genericError }
            loading = false
        }
    }

    LaunchedEffect(Unit) { refresh() }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Reveal(index = 0) {
                Row(verticalAlignment = Alignment.CenterVertically) {
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
                        bold = stringResource(R.string.conflict_title),
                    )
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
                        text = stringResource(R.string.conflict_loading),
                        style = MaterialTheme.typography.bodyLarge,
                        color = RetroSaveColors.outline,
                    )
                }
            }
        }
        error?.let { message ->
            item {
                PaperCard { CardBody(text = message, tone = RetroSaveColors.error) }
            }
        }
        if (!loading && conflicts.isEmpty()) {
            item {
                PaperCard { CardBody(stringResource(R.string.conflict_none)) }
            }
        }
        items(items = conflicts, key = RemoteConflict::id) { conflict ->
            val versions = versionsByUnit[conflict.unitId].orEmpty()
            ConflictCard(
                conflict = conflict,
                versions = versions,
                enabled = !loading,
                onKeep = { winner ->
                    scope.launch {
                        loading = true
                        error = null
                        resolve(conflict.id, winner)
                            .onSuccess { refresh() }
                            .onFailure {
                                // Un 410 signifie que quelqu'un a déjà tranché :
                                // on rafraîchit au lieu d'insister.
                                error = it.message ?: genericError
                                refresh()
                            }
                    }
                },
                modifier = Modifier.animateItem(),
            )
        }
    }
}

@Composable
private fun ConflictCard(
    conflict: RemoteConflict,
    versions: List<RemoteVersion>,
    enabled: Boolean,
    onKeep: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    PaperCard(modifier = modifier) {
        CardTitle(conflict.unitLabel)
        BranchBlock(
            label = stringResource(R.string.conflict_branch_a),
            head = conflict.versionA,
            version = versions.firstOrNull { it.number == conflict.versionA.number },
            enabled = enabled,
            onKeep = { onKeep(conflict.versionA.number) },
        )
        BranchBlock(
            label = stringResource(R.string.conflict_branch_b),
            head = conflict.versionB,
            version = versions.firstOrNull { it.number == conflict.versionB.number },
            enabled = enabled,
            onKeep = { onKeep(conflict.versionB.number) },
        )
    }
}

@Composable
private fun BranchBlock(
    label: String,
    head: ConflictHead,
    version: RemoteVersion?,
    enabled: Boolean,
    onKeep: () -> Unit,
) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Text(
            text = stringResource(R.string.conflict_branch_heading, label, head.number),
            style = MaterialTheme.typography.titleMedium,
            color = RetroSaveColors.onSurface,
        )
        Text(
            text =
                stringResource(
                    R.string.conflict_branch_metadata,
                    version?.originDeviceName ?: stringResource(R.string.conflict_unknown_device),
                    version?.createdAt ?: stringResource(R.string.conflict_unknown_date),
                    formatBytes(head.sizeBytes),
                ),
            style = MaterialTheme.typography.bodySmall,
            color = RetroSaveColors.onSurfaceVariant,
        )
        PillButton(
            text = stringResource(R.string.conflict_keep, label),
            onClick = onKeep,
            enabled = enabled,
            // Trancher un conflit est irréversible du point de vue de l'écran :
            // la vibration marque le geste.
            haptic = true,
        )
    }
}

private fun formatBytes(bytes: Long): String =
    when {
        bytes >= 1024L * 1024L -> "%.1f Mio".format(bytes / (1024.0 * 1024.0))
        bytes >= 1024L -> "%.1f Kio".format(bytes / 1024.0)
        else -> "$bytes o"
    }
