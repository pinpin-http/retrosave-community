package com.retrosave.data.icons

import com.retrosave.core.adapters.MAX_ARTWORK_BYTES
import com.retrosave.core.adapters.isValidIcon
import com.retrosave.core.sync.RemoteUnit
import io.ktor.client.HttpClient
import io.ktor.client.engine.okhttp.OkHttp
import io.ktor.client.request.get
import io.ktor.client.statement.bodyAsChannel
import io.ktor.http.isSuccess
import io.ktor.utils.io.readRemaining
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.io.readByteArray
import java.io.File

/**
 * Aller chercher les jaquettes proposées par le serveur — Q45.
 *
 * Le serveur ne stocke aucune image : il rend une **adresse**, et c'est cette
 * classe qui va chercher l'image puis la range dans le cache local. C'est ce
 * qui fait qu'une sauvegarde 3DS peut porter une vraie jaquette alors que son
 * icône vit dans la ROM — que nous ne lisons jamais (invariant I1).
 *
 * Trois propriétés, et chacune a sa raison :
 *
 * 1. **Rien ici ne peut faire échouer une synchronisation.** Une jaquette est
 *    décorative ; tout ce qui rate est silencieux.
 * 2. **Une adresse n'est tentée qu'une fois.** Sans marqueur, un jeu sans
 *    jaquette provoquerait un téléchargement raté à chaque passe. Le marqueur
 *    retient l'adresse essayée : si le serveur en propose une autre plus tard,
 *    elle sera tentée.
 * 3. **L'image est validée par son en-tête avant d'être écrite**, comme celle
 *    qui vient d'une sauvegarde. Ce qui n'est pas un PNG exploitable n'entre
 *    pas dans le cache.
 */
class ArtworkFetcher(
    private val icons: IconCache,
    private val client: HttpClient = HttpClient(OkHttp) { expectSuccess = false },
) {
    suspend fun refresh(units: List<RemoteUnit>) {
        for (unit in units) {
            val url = unit.artworkUrl ?: continue
            fetchOne(unit.emulator, unit.unitKey, url)
        }
    }

    private suspend fun fetchOne(
        emulator: String,
        unitKey: String,
        url: String,
    ): Unit =
        withContext(Dispatchers.IO) {
            val target = icons.fileFor(emulator, unitKey)
            if (target.exists()) return@withContext
            val marker = File(target.path + ".tried")
            val alreadyTried =
                try {
                    marker.exists() && marker.readText().trim() == url
                } catch (_: Exception) {
                    false
                }
            if (alreadyTried) return@withContext

            val data =
                try {
                    val response = client.get(url)
                    if (!response.status.isSuccess()) {
                        null
                    } else {
                        // Borné à la lecture : un serveur qui enverrait un
                        // fichier énorme ne doit pas pouvoir remplir le cache.
                        response
                            .bodyAsChannel()
                            .readRemaining((MAX_ARTWORK_BYTES + 1).toLong())
                            .readByteArray()
                    }
                } catch (_: Exception) {
                    null
                }

            // Le marqueur est posé dans TOUS les cas : un échec comme une
            // réussite valent « cette adresse a été tentée ».
            try {
                marker.parentFile?.mkdirs()
                marker.writeText(url)
            } catch (_: Exception) {
                // Un marqueur qu'on ne peut pas écrire ne coûte qu'une
                // nouvelle tentative ; ce n'est pas une raison de s'arrêter.
            }

            if (data == null || data.size > MAX_ARTWORK_BYTES) return@withContext
            if (!isValidIcon(data, MAX_ARTWORK_BYTES)) return@withContext
            icons.put(emulator, unitKey, data, MAX_ARTWORK_BYTES)
        }
}
