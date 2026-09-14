package com.retrosave.ui.onboarding

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import com.retrosave.R
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.CardTitle
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroSaveColors

/**
 * Réglages de la détection de session (M7 §5).
 *
 * Deux états lisibles, jamais un interrupteur qui ment. La permission
 * `PACKAGE_USAGE_STATS` peut être retirée à tout moment sans que l'application
 * en soit notifiée : cette carte lit l'état réel à chaque affichage plutôt
 * qu'un drapeau qu'on aurait persisté.
 *
 * L'interrupteur d'étage 2 a été **supprimé**, pas désactivé (Q31) : Android
 * interdit de démarrer un service de premier plan depuis l'arrière-plan, donc
 * la case ne pouvait pas tenir sa promesse. La carte annonce désormais le
 * comportement réel — au plus tard le cycle suivant — et renvoie vers la tuile
 * pour l'immédiat.
 *
 * La phrase de confidentialité est là parce que c'est une permission qui
 * inquiète à juste titre — et elle a raison de le faire. Dire exactement ce
 * qu'on lit vaut mieux que rassurer vaguement.
 */
@Composable
fun SessionSettingsCard(
    permitted: Boolean,
    onGrantPermission: () -> Unit,
    modifier: Modifier = Modifier,
) {
    PaperCard(modifier = modifier) {
        CardTitle(stringResource(R.string.settings_session_title))
        // Rien à dire quand la permission est là : la carte le montre par son
        // absence de message. Une permission manquante, elle, empêche la
        // fonction de tenir sa promesse — c'est un problème, pas une nuance.
        if (!permitted) {
            CardBody(
                text = stringResource(R.string.settings_session_missing),
                tone = RetroSaveColors.error,
            )
            PillButton(
                text = stringResource(R.string.settings_session_grant),
                onClick = onGrantPermission,
            )
        }
    }
}
