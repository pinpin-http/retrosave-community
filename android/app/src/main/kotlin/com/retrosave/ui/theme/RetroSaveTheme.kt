package com.retrosave.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Identité visuelle « Botanical Wellness & Glass » — miroir Kotlin de
 * `design/tokens.json`, lui-même relevé sur les maquettes livrées.
 *
 * Les valeurs sont recopiées à la main plutôt que chargées : une application ne
 * doit pas dépendre d'un fichier de conception pour démarrer. Elles ne peuvent
 * pas dériver en silence pour autant — `tests/test_design_tokens.py` relit ce
 * fichier et son équivalent Qt et échoue si l'un s'écarte. Même discipline que
 * les vecteurs partagés du noyau.
 *
 * Ce sont des rôles Material 3, donc la transposition est directe ici. Côté Qt
 * il a fallu les renommer : QML refuse tout identifiant `on` + majuscule, où il
 * voit un gestionnaire de signal.
 *
 * **Le thème est CLAIR.** La DA rompt explicitement avec le sombre saturé :
 * fond porcelaine, cartes blanches, encre carbone, et une gélule vert-noir pour
 * l'action principale.
 */
object RetroSaveColors {
    val background = Color(0xFFF6FBF5)
    val surfaceLowest = Color(0xFFFFFFFF)
    val surfaceLow = Color(0xFFF0F5EF)
    val surfaceContainer = Color(0xFFEAEFEA)
    val surfaceHigh = Color(0xFFE5E9E4)
    val surfaceHighest = Color(0xFFDFE4DE)
    val onSurface = Color(0xFF181D1A)
    val onSurfaceVariant = Color(0xFF424844)
    val outline = Color(0xFF727974)
    val outlineVariant = Color(0xFFC1C8C3)
    val primary = Color(0xFF07241A)
    val onPrimary = Color(0xFFFFFFFF)
    val primaryContainer = Color(0xFF1E3A2F)
    val onPrimaryContainer = Color(0xFF86A496)
    val secondary = Color(0xFF43664D)
    val secondaryContainer = Color(0xFFC2E9C9)
    val onSecondaryFixed = Color(0xFF00210E)
    val tertiaryDim = Color(0xFF4DE082)
    val error = Color(0xFFBA1A1A)
    val errorContainer = Color(0xFFFFDAD6)
    val onErrorContainer = Color(0xFF93000A)
    val inverseSurface = Color(0xFF2C322E)
    val inverseOnSurface = Color(0xFFEDF2EC)
}

/**
 * Durées de mouvement, partagées avec le desktop.
 *
 * Le mouvement explique, il ne décore pas : un survol confirme qu'un élément
 * répond, une apparition décalée fait suivre l'ordre de lecture, et un état
 * vivant respire lentement. Ce qui ne bouge pas est ce qui ne demande rien.
 */
object RetroSaveMotion {
    const val HOVER_MS = 140
    const val ENTER_MS = 320
    const val STAGGER_MS = 60
    const val PULSE_MS = 1800
}

private val colors =
    lightColorScheme(
        primary = RetroSaveColors.primary,
        onPrimary = RetroSaveColors.onPrimary,
        primaryContainer = RetroSaveColors.primaryContainer,
        onPrimaryContainer = RetroSaveColors.onPrimaryContainer,
        secondary = RetroSaveColors.secondary,
        onSecondary = RetroSaveColors.onPrimary,
        secondaryContainer = RetroSaveColors.secondaryContainer,
        onSecondaryContainer = RetroSaveColors.onSecondaryFixed,
        tertiary = RetroSaveColors.tertiaryDim,
        background = RetroSaveColors.background,
        onBackground = RetroSaveColors.onSurface,
        surface = RetroSaveColors.background,
        onSurface = RetroSaveColors.onSurface,
        surfaceVariant = RetroSaveColors.surfaceHighest,
        onSurfaceVariant = RetroSaveColors.onSurfaceVariant,
        surfaceContainerLowest = RetroSaveColors.surfaceLowest,
        surfaceContainerLow = RetroSaveColors.surfaceLow,
        surfaceContainer = RetroSaveColors.surfaceContainer,
        surfaceContainerHigh = RetroSaveColors.surfaceHigh,
        surfaceContainerHighest = RetroSaveColors.surfaceHighest,
        outline = RetroSaveColors.outline,
        outlineVariant = RetroSaveColors.outlineVariant,
        error = RetroSaveColors.error,
        onError = RetroSaveColors.onPrimary,
        errorContainer = RetroSaveColors.errorContainer,
        onErrorContainer = RetroSaveColors.onErrorContainer,
        inverseSurface = RetroSaveColors.inverseSurface,
        inverseOnSurface = RetroSaveColors.inverseOnSurface,
    )

// Galets, pas rectangles : la DA parle de courbes généreuses. `extraLarge` sert
// les grandes cartes, `full` les gélules et les chips.
private val shapes =
    Shapes(
        extraLarge = RoundedCornerShape(32.dp),
        large = RoundedCornerShape(24.dp),
        medium = RoundedCornerShape(16.dp),
        small = RoundedCornerShape(16.dp),
    )

// Plus Jakarta Sans est la police des maquettes. Le dépôt ne l'embarque pas —
// aucune application ne télécharge de police — donc Android rend l'échelle et
// les graisses avec la géométrique du système. **Graisse plafonnée à 600** :
// la DA interdit explicitement le noir gras sur les grands titres.
private val family = FontFamily.SansSerif

private val typography =
    Typography(
        displaySmall =
            TextStyle(fontFamily = family, fontSize = 40.sp, fontWeight = FontWeight.SemiBold),
        headlineLarge =
            TextStyle(fontFamily = family, fontSize = 28.sp, fontWeight = FontWeight.SemiBold),
        headlineMedium =
            TextStyle(fontFamily = family, fontSize = 22.sp, fontWeight = FontWeight.SemiBold),
        // Le couple éditorial de la DA : un mot ancré en gras, le suivant en
        // italique léger. C'est ce style-là qui porte le second mot.
        headlineSmall =
            TextStyle(
                fontFamily = family,
                fontSize = 22.sp,
                fontWeight = FontWeight.Normal,
                fontStyle = FontStyle.Italic,
            ),
        titleMedium =
            TextStyle(fontFamily = family, fontSize = 17.sp, fontWeight = FontWeight.SemiBold),
        bodyLarge = TextStyle(fontFamily = family, fontSize = 15.sp),
        bodyMedium = TextStyle(fontFamily = family, fontSize = 15.sp),
        bodySmall = TextStyle(fontFamily = family, fontSize = 13.sp),
        labelLarge =
            TextStyle(fontFamily = family, fontSize = 13.sp, fontWeight = FontWeight.SemiBold),
        labelSmall =
            TextStyle(fontFamily = family, fontSize = 11.sp, fontWeight = FontWeight.Medium),
    )

/** Thème unique, clair, sans thème dynamique dépendant du terminal. */
@Composable
fun RetroSaveTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = colors,
        shapes = shapes,
        typography = typography,
        content = content,
    )
}
