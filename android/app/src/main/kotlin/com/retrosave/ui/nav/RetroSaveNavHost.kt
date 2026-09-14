package com.retrosave.ui.nav

import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.retrosave.R
import com.retrosave.core.model.RootRef
import com.retrosave.data.adapters.AzaharIdentityChoiceException
import com.retrosave.di.APP_VERSION
import com.retrosave.di.AppContainer
import com.retrosave.sync.SyncTrigger
import com.retrosave.sync.SyncUiState
import com.retrosave.ui.conflict.ConflictScreen
import com.retrosave.ui.onboarding.AdaptersScreen
import com.retrosave.ui.onboarding.ConnectScreen
import com.retrosave.ui.theme.AuroraBackdrop
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.RetroSaveMotion
import com.retrosave.ui.theme.retroSaveEnter
import com.retrosave.ui.units.UnitDetailScreen
import com.retrosave.ui.units.UnitsListScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun RetroSaveNavHost(
    container: AppContainer,
    installedAdapters: Set<String>,
    rootError: String?,
    identityChoice: AzaharIdentityChoiceException?,
    onChooseRoot: (emulator: String, initialUri: Uri?) -> Unit,
    onSelectIdentity: (rootId: String, identity: String) -> Unit,
    onDismissRootError: () -> Unit,
) {
    val settingsState =
        container.settings.values.collectAsState(initial = null)
    val rootsState =
        container.db
            .roots()
            .observeAll()
            .collectAsState(initial = null)
    val units by
        container.db
            .units()
            .observeAll()
            .collectAsState(initial = emptyList())
    val statuses by container.adapterRegistry.statuses.collectAsState()
    val syncState by container.syncService.state.collectAsState()
    val settings = settingsState.value
    val roots = rootsState.value
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val healthError = stringResource(R.string.error_health_unavailable)

    // Le fond de l'application est le halo, pas une `Surface` Material : depuis
    // que les cartes sont blanches, `colorScheme.surface` l'est aussi, et une
    // Surface pleine page repeindrait tout l'écran en blanc.
    AuroraBackdrop {
        if (settings == null || roots == null) {
            LoadingApp()
            return@AuroraBackdrop
        }

        val navController = rememberNavController()
        val initialRoute =
            when {
                !settings.isConnected -> Routes.CONNECT
                roots.isEmpty() -> Routes.ADAPTERS
                else -> Routes.UNITS
            }

        LaunchedEffect(settings.isConnected, roots.map { it.rootId }) {
            if (settings.isConnected && roots.isNotEmpty()) {
                container.syncService.runPass(SyncTrigger.OPEN)
            }
        }

        NavHost(
            navController = navController,
            startDestination = initialRoute,
            // Le fond passe sous les barres système, le CONTENU s'arrête avant :
            // c'est l'inverse qui donne une barre d'état illisible par-dessus un
            // titre.
            modifier = Modifier.windowInsetsPadding(WindowInsets.safeDrawing),
            // Un écran ENTRE par la droite et sort vers la gauche ; le retour
            // fait l'inverse. Sans cela, la navigation coupe net et l'utilisateur
            // perd le fil de l'endroit d'où il vient.
            enterTransition = {
                slideInHorizontally(retroSaveEnter()) { width -> width / 6 } +
                    fadeIn(retroSaveEnter())
            },
            exitTransition = {
                slideOutHorizontally(retroSaveEnter(RetroSaveMotion.HOVER_MS)) { width -> -width / 12 } +
                    fadeOut(retroSaveEnter(RetroSaveMotion.HOVER_MS))
            },
            popEnterTransition = {
                slideInHorizontally(retroSaveEnter()) { width -> -width / 6 } +
                    fadeIn(retroSaveEnter())
            },
            popExitTransition = {
                slideOutHorizontally(retroSaveEnter(RetroSaveMotion.HOVER_MS)) { width -> width / 12 } +
                    fadeOut(retroSaveEnter(RetroSaveMotion.HOVER_MS))
            },
        ) {
            composable(Routes.CONNECT) {
                ConnectScreen(
                    initial = settings,
                    onConnect = { serverUrl, token, deviceName ->
                        runCatching {
                            container.settings.saveConnection(
                                serverUrl = serverUrl,
                                token = token,
                                deviceName = deviceName,
                            )
                            check(container.api.health()) {
                                healthError
                            }
                            val deviceId =
                                container.api.registerDevice(
                                    name = deviceName,
                                    osName = "android",
                                    appVersion = APP_VERSION,
                                )
                            container.settings.saveRegisteredDevice(deviceId)
                            navController.navigate(Routes.ADAPTERS) {
                                popUpTo(Routes.CONNECT) { inclusive = true }
                            }
                        }
                    },
                )
            }
            composable(Routes.ADAPTERS) {
                AdaptersScreen(
                    specs = container.adapterSpecs,
                    roots = roots,
                    statuses = statuses,
                    installedAdapters = installedAdapters,
                    error = rootError,
                    identityChoice = identityChoice,
                    onChooseRoot = onChooseRoot,
                    onSelectIdentity = onSelectIdentity,
                    onDismissError = onDismissRootError,
                    onContinue = {
                        navController.navigate(Routes.UNITS) {
                            popUpTo(Routes.ADAPTERS) { inclusive = true }
                        }
                    },
                    // Lu à chaque recomposition : la permission peut être
                    // retirée pendant que l'écran est ouvert, et un état
                    // mémorisé mentirait jusqu'au prochain lancement.
                    sessionPermitted = container.usageSessions.hasPermission(),
                    onGrantSessionPermission = {
                        context.startActivity(
                            Intent(Settings.ACTION_USAGE_ACCESS_SETTINGS)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        )
                    },
                )
            }
            composable(Routes.UNITS) {
                UnitsListScreen(
                    units = units,
                    syncState = syncState,
                    lastSuccessAtMs = settings?.lastSuccessAtMs ?: 0L,
                    lastPeriodicSuccessAtMs = settings?.lastPeriodicSuccessAtMs ?: 0L,
                    loadIcon = { unit ->
                        container.icons.iconFor(
                            vfs = container.safVfs,
                            emulator = unit.emulator,
                            unitKey = unit.unitKey,
                            root = RootRef(unit.rootId),
                            relPath = unit.relPath,
                        )
                    },
                    onSync = {
                        scope.launch {
                            container.syncService.runPass(SyncTrigger.MANUAL)
                        }
                    },
                    onResume = {
                        scope.launch {
                            container.unitStore.resumeAll()
                            container.syncService.runPass(SyncTrigger.MANUAL)
                        }
                    },
                    onAdapters = { navController.navigate(Routes.ADAPTERS) },
                    onConnection = { navController.navigate(Routes.CONNECT) },
                    onUnit = { localId ->
                        navController.navigate("${Routes.DETAIL_PREFIX}/$localId")
                    },
                    onConflicts = { navController.navigate(Routes.CONFLICT) },
                )
            }
            composable(Routes.CONFLICT) {
                ConflictScreen(
                    onBack = navController::popBackStack,
                    loadConflicts = {
                        runCatching {
                            withContext(Dispatchers.IO) { container.api.listConflicts() }
                        }
                    },
                    loadVersions = { unitId ->
                        runCatching {
                            withContext(Dispatchers.IO) { container.api.listVersions(unitId) }
                        }
                    },
                    resolve = { conflictId, winner ->
                        runCatching {
                            when (
                                val result =
                                    container.syncService.resolveConflict(
                                        conflictId = conflictId,
                                        winner = winner,
                                    )
                            ) {
                                is SyncUiState.Failed -> error(result.message)
                                else -> Unit
                            }
                        }
                    },
                )
            }
            composable(
                route = Routes.DETAIL,
                arguments =
                    listOf(
                        navArgument("localId") { type = NavType.LongType },
                    ),
            ) { entry ->
                val localId = entry.arguments?.getLong("localId") ?: return@composable
                val unit by
                    container.db
                        .units()
                        .observeByLocalId(localId)
                        .collectAsState(initial = null)
                UnitDetailScreen(
                    unit = unit,
                    onBack = navController::popBackStack,
                    loadVersions = { serverId ->
                        runCatching {
                            withContext(Dispatchers.IO) {
                                container.api.listVersions(serverId)
                            }
                        }
                    },
                    restore = { serverId, version ->
                        runCatching {
                            when (
                                val result =
                                    container.syncService.restore(
                                        unitId = serverId,
                                        version = version,
                                    )
                            ) {
                                is SyncUiState.Failed -> error(result.message)
                                else -> Unit
                            }
                        }
                    },
                    retry = { localId ->
                        withContext(Dispatchers.IO) {
                            container.unitStore.clearFailures(localId)
                            container.unitStore.resumeUnit(localId)
                        }
                    },
                )
            }
        }
    }
}

@Composable
private fun LoadingApp() {
    Column(
        modifier =
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        CircularProgressIndicator(color = RetroSaveColors.secondary)
    }
}

private object Routes {
    const val CONNECT = "connect"
    const val ADAPTERS = "adapters"
    const val UNITS = "units"
    const val CONFLICT = "conflict"
    const val DETAIL_PREFIX = "unit"
    const val DETAIL = "$DETAIL_PREFIX/{localId}"
}
