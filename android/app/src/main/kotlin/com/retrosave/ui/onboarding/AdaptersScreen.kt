package com.retrosave.ui.onboarding

import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.retrosave.R
import com.retrosave.core.adapters.AdapterSpec
import com.retrosave.data.adapters.AdapterRootStatus
import com.retrosave.data.adapters.AzaharIdentityChoiceException
import com.retrosave.data.db.entities.RootEntity
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.CardTitle
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.Reveal
import com.retrosave.ui.theme.ScreenTitle
import com.retrosave.ui.theme.StatusChip

@Composable
fun AdaptersScreen(
    specs: Map<String, AdapterSpec>,
    roots: List<RootEntity>,
    statuses: Map<String, AdapterRootStatus>,
    installedAdapters: Set<String>,
    error: String?,
    identityChoice: AzaharIdentityChoiceException?,
    onChooseRoot: (emulator: String, initialUri: Uri?) -> Unit,
    onSelectIdentity: (rootId: String, identity: String) -> Unit,
    onDismissError: () -> Unit,
    onContinue: () -> Unit,
    sessionPermitted: Boolean,
    onGrantSessionPermission: () -> Unit,
) {
    val ordered = listOf("ppsspp", "melonds", "azahar")
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            Reveal(index = 0) {
                ScreenTitle(
                    bold = stringResource(R.string.adapters_title),
                )
            }
        }
        item {
            Reveal(index = 1) {
                SessionSettingsCard(
                    permitted = sessionPermitted,
                    onGrantPermission = onGrantSessionPermission,
                )
            }
        }
        error?.let { message ->
            item {
                PaperCard {
                    CardBody(text = message, tone = RetroSaveColors.error)
                    PillButton(
                        text = stringResource(R.string.dismiss),
                        onClick = onDismissError,
                        quiet = true,
                    )
                }
            }
        }
        identityChoice?.let { choice ->
            item {
                IdentityChoiceCard(
                    choice = choice,
                    onSelect = { onSelectIdentity(choice.rootId, it) },
                )
            }
        }
        items(
            items = ordered.mapNotNull(specs::get),
            key = AdapterSpec::id,
        ) { spec ->
            val root = roots.singleOrNull { it.emulator == spec.id }
            val status = root?.let { statuses[it.rootId] }
            AdapterCard(
                spec = spec,
                root = root,
                status = status,
                installed = spec.id in installedAdapters,
                onChooseRoot = {
                    onChooseRoot(spec.id, root?.treeUri?.let(Uri::parse))
                },
                modifier = Modifier.animateItem(),
            )
        }
        if (roots.isNotEmpty()) {
            item {
                PillButton(
                    text = stringResource(R.string.adapters_continue),
                    onClick = onContinue,
                    modifier = Modifier.fillMaxWidth(),
                    quiet = true,
                    quietInk = RetroSaveColors.onSurface,
                )
            }
        }
    }
}

@Composable
private fun AdapterCard(
    spec: AdapterSpec,
    root: RootEntity?,
    status: AdapterRootStatus?,
    installed: Boolean,
    onChooseRoot: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PaperCard(modifier = modifier) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CardTitle(spec.name)
            // Le mot ET la couleur : une pastille verte seule ne dit rien à
            // qui ne distingue pas les teintes, ni en plein soleil (EXP-06).
            StatusChip(
                label =
                    stringResource(
                        if (installed) R.string.adapter_installed else R.string.adapter_not_detected,
                    ),
                tint =
                    if (installed) RetroSaveColors.secondary else RetroSaveColors.onSurfaceVariant,
                paper = true,
            )
        }
        CardBody(
            text = rootStatus(root, status),
            tone = grantTone(status),
        )
        status?.warning?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = RetroSaveColors.onSurfaceVariant,
            )
        }
        status?.activeIdentity?.let {
            Text(
                text = stringResource(R.string.adapter_active_identity, it),
                style = MaterialTheme.typography.bodySmall,
                color = RetroSaveColors.onSurfaceVariant,
            )
        }
        status?.ignoredIdentities?.takeIf(List<String>::isNotEmpty)?.let {
            Text(
                text = stringResource(R.string.adapter_ignored_identities, it.joinToString()),
                style = MaterialTheme.typography.bodySmall,
                color = RetroSaveColors.onSurfaceVariant,
            )
        }
        PillButton(
            text =
                stringResource(
                    if (root == null) {
                        R.string.adapter_choose_folder
                    } else {
                        R.string.adapter_reauthorize_folder
                    },
                ),
            onClick = onChooseRoot,
        )
    }
}

/**
 * La couleur d'un état d'accès. Le vert ne dit qu'une chose — l'accès est
 * accordé et le dossier lisible. Un accès révoqué est un problème, pas un
 * avertissement : c'est le rouge « sur carte ».
 */
private fun grantTone(status: AdapterRootStatus?): Color =
    when {
        status == null -> RetroSaveColors.onSurfaceVariant
        !status.accessible -> RetroSaveColors.error
        else -> RetroSaveColors.secondary
    }

@Composable
private fun rootStatus(
    root: RootEntity?,
    status: AdapterRootStatus?,
): String =
    when {
        root == null -> stringResource(R.string.adapter_grant_missing)
        status == null -> stringResource(R.string.adapter_grant_saved)
        !status.accessible -> stringResource(R.string.adapter_grant_revoked)
        else -> stringResource(R.string.adapter_grant_ok, status.unitCount)
    }

@Composable
private fun IdentityChoiceCard(
    choice: AzaharIdentityChoiceException,
    onSelect: (String) -> Unit,
) {
    PaperCard {
        CardTitle(stringResource(R.string.identity_choice_title))
        choice.identities.forEach { option ->
            val proposed =
                if (option.identity == choice.proposedIdentity) {
                    stringResource(R.string.identity_proposed_suffix)
                } else {
                    ""
                }
            PillButton(
                text = "${option.identity}$proposed",
                onClick = { onSelect(option.identity) },
                modifier = Modifier.fillMaxWidth(),
                quiet = true,
            )
        }
    }
}
