package com.retrosave.ui.theme

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * La marque : une étoile à sept branches, dessinée plutôt qu'importée.
 *
 * Même géométrie que `desktop/qml/StarMark.qml` — sept sommets, rapport
 * intérieur 0,42, décalage d'un quart de tour pour qu'une branche pointe vers le
 * haut. Une image aurait été un binaire de plus à mettre à l'échelle et à garder
 * synchronisé entre les deux clients.
 */
@Composable
fun RetroSaveStar(
    modifier: Modifier = Modifier,
    color: Color = RetroSaveColors.primary,
    points: Int = 7,
    innerRatio: Float = 0.42f,
) {
    Canvas(modifier = modifier) {
        val outer = min(size.width, size.height) / 2f
        val inner = outer * innerRatio
        val center = Offset(size.width / 2f, size.height / 2f)
        val path = Path()
        // Un sommet, un creux, et ainsi de suite : 2n points au total.
        for (step in 0 until points * 2) {
            val radius = if (step % 2 == 0) outer else inner
            val angle = (PI * step / points - PI / 2).toFloat()
            val x = center.x + radius * cos(angle)
            val y = center.y + radius * sin(angle)
            if (step == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        path.close()
        drawPath(path, color)
    }
}
