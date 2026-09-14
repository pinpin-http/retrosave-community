package com.retrosave.ui.theme

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.animateIntAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.LocalIndication
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

// ─── Le vocabulaire visuel d'Android ────────────────────────────────────
// Même identité « Botanical Wellness & Glass » que le desktop Qt, transposée
// en Compose.
//
// **Le thème est CLAIR** : fond porcelaine, cartes blanches, encre carbone, et
// une gélule vert-noir pour l'action principale. Ce fichier ne connaît aucune
// couleur en propre : tout vient de [RetroSaveColors], miroir de
// `design/tokens.json` que `tests/test_design_tokens.py` compare au thème Qt.
//
// | Élément | Forme |
// |---|---|
// | Carte | Blanche, rayon des jetons |
// | Carte d'accent | Vert doux, encre verte sombre — une seule par écran |
// | Chiffres | Tuiles, sur la carte d'accent |
// | Action principale | Gélule pleine vert-noir |
// | Action secondaire | Cercle ou gélule calme |
// | Titre d'écran | Un mot dense, puis un mot en italique léger |
//
// La hiérarchie tient à la MATIÈRE, pas à la taille.
//

/**
 * ─── Mouvement ───
 *
 * L'entrée commune : la durée des jetons, partagée avec le desktop.
 *
 * Rendue générique parce qu'elle sert aussi bien à un flottant (opacité) qu'à
 * un entier (compteur) ou à un décalage de navigation.
 */
fun <T> retroSaveEnter(durationMillis: Int = RetroSaveMotion.ENTER_MS) = tween<T>(durationMillis)

/**
 * La compression au doigt, branchée sur la source d'interaction d'un bouton.
 *
 * Passer par [InteractionSource] plutôt que par un `pointerInput` maison évite
 * de réimplémenter la détection d'appui — et surtout de la voir diverger de
 * celle que Material utilise déjà pour l'ondulation.
 */
@Composable
fun Modifier.pressScale(interactionSource: InteractionSource): Modifier {
    val pressed by interactionSource.collectIsPressedAsState()
    val scale by
        animateFloatAsState(
            targetValue = if (pressed) 0.97f else 1f,
            animationSpec = tween(RetroSaveMotion.HOVER_MS),
            label = "pression",
        )
    return graphicsLayer {
        scaleX = scale
        scaleY = scale
    }
}

/**
 * L'entrée en cascade : un fondu et une courte montée, retardés de
 * [index] × [RetroSaveMotion.STAGGER_MS].
 *
 * À réserver aux éléments FIXES d'un écran. Dans une liste paresseuse, un
 * élément recomposé en revenant en arrière rejouerait son entrée à chaque
 * passage — précisément l'effet bon marché qu'on cherche à éviter. Les listes
 * utilisent `Modifier.animateItem()`, qui n'anime que les vrais changements.
 */
@Composable
fun Reveal(
    index: Int = 0,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(index.toLong() * RetroSaveMotion.STAGGER_MS)
        shown = true
    }
    val progress by
        animateFloatAsState(
            targetValue = if (shown) 1f else 0f,
            animationSpec = retroSaveEnter(),
            label = "enter",
        )
    val rise = with(LocalDensity.current) { 16.dp.toPx() }
    Box(
        modifier =
            modifier.graphicsLayer {
                alpha = progress
                translationY = (1f - progress) * rise
            },
    ) {
        content()
    }
}

// ─── Le fond ──────────────────────────────────────────────────────────────

/**
 * Le fond de tous les écrans : un aplat porcelaine.
 *
 * Le thème étant clair, il n'y a **pas** de halo sombre à peindre : la
 * profondeur vient des cartes blanches posées dessus, pas d'un dégradé. Le
 * composant reste en place parce qu'il est le seul endroit où le fond est
 * décidé — et parce qu'un écran ne doit jamais poser une `Surface` Material
 * pleine page, qui repeindrait tout en blanc et effacerait cette distinction.
 */
@Composable
fun AuroraBackdrop(
    modifier: Modifier = Modifier,
    content: @Composable BoxScope.() -> Unit,
) {
    Box(
        modifier = modifier.fillMaxSize().background(RetroSaveColors.background),
        content = content,
    )
}

// ─── Les surfaces ─────────────────────────────────────────────────────────

/** La carte ordinaire : blanche, sur le fond porcelaine. */
@Composable
fun PaperCard(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    // L'ORDRE compte : la compression enveloppe la carte, la découpe et le fond
    // viennent ensuite, et le clic — donc l'ondulation — arrive en dernier pour
    // être dessiné DANS la carte. Inversé, le halo de l'appui passerait sous le
    // blanc et on ne verrait plus rien.
    val surface =
        modifier
            .fillMaxWidth()
            .then(if (onClick == null) Modifier else Modifier.pressScale(interaction))
            .clip(MaterialTheme.shapes.large)
            .background(RetroSaveColors.surfaceLowest)
            .border(1.dp, RetroSaveColors.outlineVariant, MaterialTheme.shapes.large)
            .then(
                if (onClick == null) {
                    Modifier
                } else {
                    Modifier.clickable(
                        interactionSource = interaction,
                        indication = LocalIndication.current,
                        onClick = onClick,
                    )
                },
            ).padding(20.dp)
    Column(
        modifier = surface,
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

/**
 * La seule carte pleine d'un écran. Elle porte l'état principal : le vert dit
 * « en sûreté », et une seule surface verte suffit à le dire. Deux, et l'œil ne
 * sait plus où regarder.
 */
@Composable
fun AccentCard(
    modifier: Modifier = Modifier,
    working: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    // Une passe en cours fait respirer la carte, très lentement. C'est la seule
    // façon honnête de montrer un travail dont on ne connaît pas l'avancement :
    // une barre de progression prétendrait le savoir.
    val breath by
        rememberInfiniteTransition(label = "travail").animateFloat(
            initialValue = 0.86f,
            targetValue = 1f,
            animationSpec =
                infiniteRepeatable(
                    animation = tween(RetroSaveMotion.PULSE_MS),
                    repeatMode = RepeatMode.Reverse,
                ),
            label = "travail",
        )
    Column(
        modifier =
            modifier
                .fillMaxWidth()
                .alpha(if (working) breath else 1f)
                .clip(MaterialTheme.shapes.large)
                .background(RetroSaveColors.secondaryContainer)
                .padding(20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

/** Une valeur et son nom, en tuile. Le chiffre MONTE au lieu de sauter. */
@Composable
fun StatTile(
    value: Int,
    caption: String,
    modifier: Modifier = Modifier,
) {
    // Le compteur qui grimpe est ce qui fait remarquer qu'une passe vient de se
    // terminer — sans notification, sans texte à lire.
    val shown by
        animateIntAsState(
            targetValue = value,
            animationSpec = retroSaveEnter(),
            label = "compteur",
        )
    Column(
        modifier =
            modifier
                .clip(RoundedCornerShape(14.dp))
                .background(RetroSaveColors.surfaceLowest)
                .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Text(
            text = shown.toString(),
            style = MaterialTheme.typography.titleLarge,
            color = RetroSaveColors.onSurface,
        )
        Text(
            text = caption,
            style = MaterialTheme.typography.bodySmall,
            color = RetroSaveColors.onSurfaceVariant,
        )
    }
}

/**
 * Puce d'état : la couleur ET le mot. La couleur ne porte jamais seule
 * l'information (EXP-06) — c'est aussi ce qui la rend lisible en plein soleil
 * sur une console portable.
 */
@Composable
fun StatusChip(
    label: String,
    tint: Color,
    modifier: Modifier = Modifier,
    alive: Boolean = false,
    paper: Boolean = true,
) {
    val pulse by
        rememberInfiniteTransition(label = "pouls").animateFloat(
            initialValue = 0.4f,
            targetValue = 1f,
            animationSpec =
                infiniteRepeatable(
                    animation = tween(RetroSaveMotion.PULSE_MS),
                    repeatMode = RepeatMode.Reverse,
                ),
            label = "pouls",
        )
    Row(
        modifier =
            modifier
                .clip(CircleShape)
                .background(
                    if (paper) RetroSaveColors.surfaceHigh else RetroSaveColors.surfaceContainer,
                ).padding(horizontal = 12.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier =
                Modifier
                    .size(7.dp)
                    .alpha(if (alive) pulse else 1f)
                    .clip(CircleShape)
                    .background(tint),
        )
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = RetroSaveColors.onSurface,
        )
    }
}

// ─── Les commandes ────────────────────────────────────────────────────────

/**
 * L'action principale : une gélule PLEINE vert-noir. Elle se comprime sous le
 * doigt — assez pour que la main sente une réponse, trop peu pour qu'on le
 * remarque.
 *
 * [haptic] est réservé aux gestes qui ENGAGENT quelque chose : lancer une
 * passe, trancher un conflit, restaurer. Une vibration sur chaque bouton finit
 * par ne plus rien signifier.
 */
@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    quiet: Boolean = false,
    quietInk: Color = RetroSaveColors.onSurface,
    haptic: Boolean = false,
) {
    val interaction = remember { MutableInteractionSource() }
    val feedback = LocalHapticFeedback.current
    val press = {
        if (haptic) feedback.performHapticFeedback(HapticFeedbackType.LongPress)
        onClick()
    }
    val shaped = modifier.pressScale(interaction)
    if (quiet) {
        OutlinedButton(
            onClick = press,
            modifier = shaped,
            enabled = enabled,
            shape = CircleShape,
            interactionSource = interaction,
            border = BorderStroke(1.dp, RetroSaveColors.outlineVariant),
            colors =
                ButtonDefaults.outlinedButtonColors(
                    containerColor = RetroSaveColors.surfaceLow,
                    contentColor = quietInk,
                    disabledContentColor = RetroSaveColors.outline,
                ),
        ) {
            Text(text, style = MaterialTheme.typography.titleSmall)
        }
        return
    }
    Button(
        onClick = press,
        modifier = shaped,
        enabled = enabled,
        shape = CircleShape,
        interactionSource = interaction,
        colors =
            ButtonDefaults.buttonColors(
                containerColor = RetroSaveColors.primary,
                contentColor = RetroSaveColors.onPrimary,
                disabledContainerColor = RetroSaveColors.surfaceHigh,
                disabledContentColor = RetroSaveColors.outline,
            ),
    ) {
        Text(text, style = MaterialTheme.typography.labelLarge)
    }
}

/**
 * Action secondaire : un CERCLE. Elle ne rivalise jamais visuellement avec
 * l'action principale, parce qu'elle n'a pas la même forme.
 */
@Composable
fun CircleButton(
    glyph: String,
    onClick: () -> Unit,
    description: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    paper: Boolean = true,
) {
    val interaction = remember { MutableInteractionSource() }
    Surface(
        onClick = onClick,
        modifier =
            modifier
                .size(46.dp)
                .pressScale(interaction)
                .semantics { this.contentDescription = description },
        enabled = enabled,
        shape = CircleShape,
        color = if (paper) RetroSaveColors.surfaceLow else RetroSaveColors.surfaceContainer,
        border = BorderStroke(1.dp, RetroSaveColors.outlineVariant),
        interactionSource = interaction,
    ) {
        // Sans `fillMaxSize`, la boîte se réduit au glyphe et se colle en haut à
        // gauche du cercle au lieu d'en occuper le centre.
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(
                text = glyph,
                color = if (enabled) RetroSaveColors.onSurface else RetroSaveColors.outline,
                style = MaterialTheme.typography.titleLarge,
            )
        }
    }
}

/**
 * Champ : une gélule creusée dans la carte. Le liseré passe au vert au focus —
 * le seul endroit où la couleur dit « ici ».
 */
@Composable
fun RetroField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    visualTransformation: VisualTransformation = VisualTransformation.None,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier.fillMaxWidth(),
        label = { Text(label) },
        singleLine = true,
        enabled = enabled,
        shape = MaterialTheme.shapes.medium,
        visualTransformation = visualTransformation,
        colors =
            OutlinedTextFieldDefaults.colors(
                focusedTextColor = RetroSaveColors.onSurface,
                unfocusedTextColor = RetroSaveColors.onSurface,
                disabledTextColor = RetroSaveColors.outline,
                focusedContainerColor = RetroSaveColors.surfaceLowest,
                unfocusedContainerColor = RetroSaveColors.surfaceLow,
                disabledContainerColor = RetroSaveColors.surfaceLow,
                cursorColor = RetroSaveColors.secondary,
                focusedBorderColor = RetroSaveColors.secondary,
                unfocusedBorderColor = RetroSaveColors.outlineVariant,
                focusedLabelColor = RetroSaveColors.secondary,
                unfocusedLabelColor = RetroSaveColors.onSurfaceVariant,
            ),
    )
}

// ─── Le texte ─────────────────────────────────────────────────────────────

/**
 * Le titre d'un écran, et un sous-titre facultatif.
 *
 * Le sous-titre ne porte que ce qu'on ne peut pas deviner du titre — sur le
 * détail d'une sauvegarde, son émulateur et sa clé. Pas d'accroche.
 */
@Composable
fun ScreenTitle(
    bold: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
) {
    Column(modifier = modifier.fillMaxWidth()) {
        Text(
            text = bold,
            style = MaterialTheme.typography.headlineLarge,
            color = RetroSaveColors.onSurface,
        )
        if (!subtitle.isNullOrEmpty()) {
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodyLarge,
                color = RetroSaveColors.onSurfaceVariant,
            )
        }
    }
}

/** Titre d'une carte blanche. */
@Composable
fun CardTitle(
    text: String,
    modifier: Modifier = Modifier,
) {
    Text(
        text = text,
        modifier = modifier,
        style = MaterialTheme.typography.titleLarge,
        color = RetroSaveColors.onSurface,
    )
}

/** Texte courant d'une carte blanche. */
@Composable
fun CardBody(
    text: String,
    modifier: Modifier = Modifier,
    tone: Color = RetroSaveColors.onSurfaceVariant,
) {
    Text(
        text = text,
        modifier = modifier,
        style = MaterialTheme.typography.bodyLarge,
        color = tone,
    )
}

/** Intertitre posé sur le fond, entre deux groupes de cartes. */
@Composable
fun SectionHeading(
    text: String,
    modifier: Modifier = Modifier,
) {
    Text(
        text = text,
        modifier = modifier.padding(top = 8.dp, start = 4.dp),
        style = MaterialTheme.typography.titleLarge,
        color = RetroSaveColors.onSurface,
        fontStyle = FontStyle.Italic,
        fontWeight = FontWeight.Light,
    )
}
