package com.retrosave

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import com.retrosave.data.adapters.AzaharIdentityChoiceException
import com.retrosave.di.AppContainer
import com.retrosave.ui.nav.RetroSaveNavHost
import com.retrosave.ui.theme.RetroSaveTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Activité unique : le picker SAF reste ici, la navigation et les écrans dans ui/. */
class MainActivity : ComponentActivity() {
    private val container: AppContainer
        get() = (application as RetroSaveApp).container

    override fun onCreate(savedInstanceState: Bundle?) {
        // Bord à bord : le halo vert monte jusque sous la barre d'état et la
        // barre de navigation. Sans cela l'écran est encadré de deux bandes
        // opaques, et le fond ne ressemble plus à rien. Les écrans, eux,
        // respectent les encoches (voir `RetroSaveNavHost`).
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        setContent {
            RetroSaveTheme {
                val scope = rememberCoroutineScope()
                var pendingEmulator by remember { mutableStateOf<String?>(null) }
                var rootError by remember { mutableStateOf<String?>(null) }
                var identityChoice by
                    remember {
                        mutableStateOf<AzaharIdentityChoiceException?>(null)
                    }
                val picker =
                    rememberLauncherForActivityResult(
                        contract = ActivityResultContracts.OpenDocumentTree(),
                    ) { uri ->
                        val emulator = pendingEmulator
                        pendingEmulator = null
                        if (uri != null && emulator != null) {
                            scope.launch {
                                rootError = null
                                identityChoice = null
                                registerRoot(emulator, uri)
                                    .onFailure { error ->
                                        if (error is AzaharIdentityChoiceException) {
                                            identityChoice = error
                                        } else {
                                            rootError =
                                                error.message
                                                    ?: getString(R.string.error_root_registration)
                                        }
                                    }
                            }
                        }
                    }

                RetroSaveNavHost(
                    container = container,
                    installedAdapters = remember { installedAdapters() },
                    rootError = rootError,
                    identityChoice = identityChoice,
                    onChooseRoot = { emulator, initialUri ->
                        rootError = null
                        identityChoice = null
                        pendingEmulator = emulator
                        picker.launch(initialUri)
                    },
                    onSelectIdentity = { rootId, identity ->
                        scope.launch {
                            rootError = null
                            identityChoice = null
                            runCatching {
                                withContext(Dispatchers.IO) {
                                    container.rootRepository.selectAzaharIdentity(
                                        rootId = rootId,
                                        identity = identity,
                                    )
                                    container.adapterRegistry.discover()
                                }
                            }.onFailure { error ->
                                rootError =
                                    error.message
                                        ?: getString(R.string.error_identity_selection)
                            }
                        }
                    },
                    onDismissRootError = {
                        rootError = null
                        identityChoice = null
                    },
                )
            }
        }
    }

    private suspend fun registerRoot(
        emulator: String,
        uri: Uri,
    ): Result<Unit> {
        val flags =
            Intent.FLAG_GRANT_READ_URI_PERMISSION or
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        return try {
            contentResolver.takePersistableUriPermission(
                uri,
                flags,
            )
            withContext(Dispatchers.IO) {
                container.rootRepository.register(
                    emulator = emulator,
                    treeUri = uri,
                    label = container.adapterSpecs.getValue(emulator).name,
                )
                container.adapterRegistry.discover()
            }
            Result.success(Unit)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            val registered =
                withContext(Dispatchers.IO) {
                    container.db
                        .roots()
                        .listByEmulator(emulator)
                        .any { it.treeUri == uri.toString() }
                }
            if (!registered) {
                try {
                    contentResolver.releasePersistableUriPermission(uri, flags)
                } catch (_: SecurityException) {
                    // Le provider peut déjà avoir retiré le grant : aucun état à réparer.
                }
            }
            Result.failure(error)
        }
    }

    @Suppress("DEPRECATION")
    private fun installedAdapters(): Set<String> =
        container.adapterSpecs
            .filterValues { spec ->
                spec.androidPackages.any { packageName ->
                    try {
                        packageManager.getPackageInfo(packageName, 0)
                        true
                    } catch (_: PackageManager.NameNotFoundException) {
                        false
                    }
                }
            }.keys
}
