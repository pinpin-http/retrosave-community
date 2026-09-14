package com.retrosave.ui.units

import android.graphics.BitmapFactory
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import com.retrosave.core.adapters.placeholderHue
import com.retrosave.core.adapters.placeholderInitials
import androidx.compose.foundation.Image as ComposeImage

/**
 * Vignette d'une unité — M8 §8.
 *
 * Trois sources et trois seulement : l'icône contenue dans la sauvegarde, un
 * placeholder calculé, et plus tard une image importée. Jamais de banque de
 * jaquettes téléchargée.
 *
 * La dégradation est **assumée et cohérente** : une unité qui n'existe que sur
 * l'autre appareil n'a pas d'icône ici, mais son placeholder est calculé du
 * `gameKey` et sera donc identique sur les deux écrans. Ça reste la même
 * bibliothèque, pas un affichage cassé.
 */
@Composable
fun GameThumbnail(
    gameKey: String,
    label: String,
    icon: ByteArray?,
    modifier: Modifier = Modifier,
    // L'émulateur d'où vient la sauvegarde. Sa silhouette est posée en badge,
    // exactement comme sur le bureau : la même bibliothèque doit se reconnaître
    // d'un écran à l'autre.
    emulator: String = "folder",
) {
    // Le même rayon que les tuiles de chiffres : deux rayons voisins mais
    // différents se voient, et donnent l'impression d'un assemblage.
    val shape = RoundedCornerShape(18.dp)
    var bitmap by remember(icon) { mutableStateOf<ImageBitmap?>(null) }

    LaunchedEffect(icon) {
        // Le décodage est la seule étape qui peut encore échouer après la
        // validation d'en-tête : un PNG dont la signature est bonne peut avoir
        // des données abîmées. On retombe sur le placeholder, sans bruit.
        bitmap =
            icon?.let { raw ->
                runCatching { BitmapFactory.decodeByteArray(raw, 0, raw.size) }
                    .getOrNull()
                    ?.asImageBitmap()
            }
    }

    val decoded = bitmap
    // Une seule boîte pour les deux cas : l'image et le repli portent le même
    // badge, à la même place. Les traiter séparément l'aurait fait glisser dès
    // qu'une icône apparaît.
    Box(modifier = modifier.size(48.dp)) {
        if (decoded != null) {
            ComposeImage(
                bitmap = decoded,
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.size(48.dp).clip(shape),
            )
        } else {
            Box(
                modifier = Modifier.size(48.dp).clip(shape).background(placeholderColor(gameKey)),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = placeholderInitials(label),
                    color = Color.White,
                    style = MaterialTheme.typography.labelLarge,
                )
            }
        }

        // Le badge, dans le coin bas-droit et légèrement débordant — les mêmes
        // proportions que `SaveArtwork.qml` : 42 % de la vignette, silhouette à
        // 62 % du badge.
        Box(
            modifier =
                Modifier
                    .align(Alignment.BottomEnd)
                    .offset(x = 2.dp, y = 2.dp)
                    .size(20.dp)
                    .clip(RoundedCornerShape(6.dp))
                    .background(MaterialTheme.colorScheme.surface),
            contentAlignment = Alignment.Center,
        ) {
            EmulatorMark(
                emulator = emulator,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(3.dp).size(14.dp),
            )
        }
    }
}

/**
 * Teinte déterministe du `gameKey`, en saturation et luminosité fixes.
 *
 * Fixer les deux autres composantes garantit que le texte blanc reste lisible
 * quelle que soit la teinte tirée — un placeholder illisible ne vaut pas mieux
 * qu'une case vide.
 */
private fun placeholderColor(gameKey: String): Color =
    Color.hsv(
        hue = placeholderHue(gameKey).toFloat(),
        saturation = 0.55f,
        value = 0.45f,
    )
