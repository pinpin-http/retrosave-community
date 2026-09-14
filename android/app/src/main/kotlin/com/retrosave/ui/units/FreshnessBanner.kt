package com.retrosave.ui.units

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.retrosave.R
import com.retrosave.core.sync.periodicSyncIsStale
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.RetroSaveColors

/**
 * Fraîcheur vérifiée — AD-36 (Q32).
 *
 * Deux choses, dans cet ordre d'importance. La date de la dernière
 * vérification, toujours affichée : « à jour » sans date veut dire « je n'ai
 * rien à envoyer », pas « j'ai vérifié ». Puis l'alerte, quand la
 * synchronisation **automatique** n'a rien fait depuis plus de 24 h.
 *
 * L'alerte ne regarde jamais la passe à l'ouverture. C'est elle qui masque la
 * panne aujourd'hui, en remettant tout à jour au moment précis où l'utilisateur
 * regarde — si elle éteignait le bandeau, la synchronisation de fond pourrait
 * être morte depuis des semaines sans que rien ne le dise.
 */
@Composable
fun FreshnessBanner(
    lastSuccessAtMs: Long,
    lastPeriodicSuccessAtMs: Long,
    nowMs: Long,
    modifier: Modifier = Modifier,
) {
    val stale = periodicSyncIsStale(lastPeriodicSuccessAtMs, nowMs)
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        // La date se lit SUR le fond, pas sur une carte : c'est une note de bas
        // de page permanente, pas une information à peser.
        Text(
            text =
                if (lastSuccessAtMs <= 0L) {
                    stringResource(R.string.freshness_never)
                } else {
                    stringResource(R.string.freshness_checked, elapsedLabel(nowMs - lastSuccessAtMs))
                },
            modifier = Modifier.padding(start = 4.dp),
            style = MaterialTheme.typography.bodySmall,
            color = RetroSaveColors.outline,
        )
        if (stale) {
            // L'alerte, elle, prend une carte blanche : elle demande une action.
            // Le rouge employé est celui « sur carte » — le rouge du fond y
            // serait illisible.
            PaperCard {
                CardBody(
                    text = stringResource(R.string.freshness_stale),
                    tone = RetroSaveColors.error,
                )
            }
        }
    }
}

/** Durée lisible, volontairement grossière : la précision n'apporte rien ici. */
@Composable
fun elapsedLabel(elapsedMs: Long): String {
    val seconds = elapsedMs / 1000
    return when {
        seconds < 90 -> stringResource(R.string.freshness_just_now)
        seconds < 3600 -> stringResource(R.string.freshness_minutes, (seconds / 60).toInt())
        seconds < 48 * 3600 -> stringResource(R.string.freshness_hours, (seconds / 3600).toInt())
        else -> stringResource(R.string.freshness_days, (seconds / 86400).toInt())
    }
}
