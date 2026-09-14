package com.retrosave.ui.units

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Fill

/**
 * La marque d'un émulateur, dessinée — jamais importée.
 *
 * Portage exact de `desktop/qml/EmulatorMark.qml` : mêmes proportions, mêmes
 * silhouettes, pour que la bibliothèque se reconnaisse d'un écran à l'autre.
 * Un joueur qui voit une cartouche sur son PC doit voir la même cartouche sur
 * sa console, au pixel de géométrie près.
 *
 * Ce sont des **silhouettes d'objets**, pas des logos : reproduire la marque
 * d'un émulateur suggérerait un partenariat qui n'existe pas, et les
 * conditions de réutilisation de ces logos sont hétérogènes (Q45).
 *
 * Rien n'est téléchargé et aucune ressource binaire n'est ajoutée : une image
 * par émulateur serait un fichier de plus à mettre à l'échelle pour chaque
 * densité d'écran.
 */
@Composable
fun EmulatorMark(
    emulator: String,
    color: Color,
    modifier: Modifier = Modifier,
) {
    Canvas(modifier = modifier) { drawEmulatorMark(emulator, color) }
}

/** Un rectangle à coins arrondis, exprimé en FRACTIONS de la surface. */
private fun DrawScope.round(
    x: Float,
    y: Float,
    rw: Float,
    rh: Float,
    radius: Float,
    color: Color,
    blend: BlendMode = BlendMode.SrcOver,
) {
    drawRoundRect(
        color = color,
        topLeft = Offset(size.width * x, size.height * y),
        size = Size(size.width * rw, size.height * rh),
        cornerRadius = CornerRadius(size.width * radius),
        style = Fill,
        blendMode = blend,
    )
}

private fun DrawScope.drawEmulatorMark(
    emulator: String,
    color: Color,
) {
    val w = size.width
    val h = size.height
    // `BlendMode.Clear` remplace le `destination-out` du Canvas QML : c'est ce
    // qui creuse une encoche dans une forme déjà posée.
    val clear = BlendMode.Clear

    when (emulator) {
        // Deux écrans empilés : la silhouette de la DS.
        "melonds" -> {
            round(0.18f, 0.12f, 0.64f, 0.34f, 0.08f, color)
            round(0.18f, 0.54f, 0.64f, 0.34f, 0.08f, color)
        }
        // Le clapet de la 3DS : l'écran haut plus large que le bas.
        "azahar" -> {
            round(0.12f, 0.12f, 0.76f, 0.34f, 0.08f, color)
            round(0.24f, 0.54f, 0.52f, 0.34f, 0.08f, color)
        }
        // La barre horizontale d'une console à écran large.
        "ppsspp" -> round(0.06f, 0.28f, 0.88f, 0.44f, 0.2f, color)
        // Un disque optique : le GameCube et la Wii lisent des galettes.
        "dolphin" -> {
            drawCircle(color, radius = w * 0.42f, center = Offset(w / 2, h / 2))
            drawCircle(color, radius = w * 0.13f, center = Offset(w / 2, h / 2), blendMode = clear)
        }
        // Une carte mémoire : un corps et son ergot de détrompage. PCSX2
        // reprend la même silhouette, en plus large — deux générations du
        // même objet.
        "duckstation", "pcsx2" -> {
            val wide = emulator == "pcsx2"
            round(if (wide) 0.08f else 0.16f, 0.2f, if (wide) 0.84f else 0.68f, 0.6f, 0.09f, color)
            round(if (wide) 0.3f else 0.34f, 0.2f, 0.32f, 0.14f, 0.03f, color, clear)
        }
        // Une console à écran unique, tenue à deux mains.
        "mgba" -> {
            round(0.06f, 0.24f, 0.88f, 0.52f, 0.18f, color)
            round(0.32f, 0.36f, 0.36f, 0.28f, 0.04f, color, clear)
        }
        // Une cartouche : le corps, et l'encoche du connecteur en bas.
        "snes9x" -> {
            round(0.18f, 0.1f, 0.64f, 0.66f, 0.07f, color)
            round(0.34f, 0.1f, 0.32f, 0.16f, 0.03f, color, clear)
            round(0.3f, 0.78f, 0.4f, 0.12f, 0.03f, color)
        }
        // Un noyau et ses satellites : le multi-système.
        "retroarch" -> {
            drawCircle(color, radius = w * 0.2f, center = Offset(w / 2, h / 2))
            repeat(3) { step ->
                val angle = (Math.PI * 2 * step / 3 - Math.PI / 2).toFloat()
                drawCircle(
                    color = color,
                    radius = w * 0.1f,
                    center =
                        Offset(
                            w / 2 + kotlin.math.cos(angle) * w * 0.35f,
                            h / 2 + kotlin.math.sin(angle) * h * 0.35f,
                        ),
                )
            }
        }
        // Le dossier générique : une languette et un corps.
        else -> {
            round(0.1f, 0.2f, 0.34f, 0.12f, 0.04f, color)
            round(0.1f, 0.28f, 0.8f, 0.5f, 0.07f, color)
        }
    }
}
