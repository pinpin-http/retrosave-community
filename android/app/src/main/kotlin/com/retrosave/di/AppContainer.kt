package com.retrosave.di

import android.app.Application
import androidx.room.Room
import com.retrosave.core.adapters.azahar.AzaharSaveDetector
import com.retrosave.core.sync.SyncEngine
import com.retrosave.core.sync.unitsBehindCloud
import com.retrosave.data.adapters.AdapterManifestLoader
import com.retrosave.data.adapters.AndroidAdapterRegistry
import com.retrosave.data.adapters.RootRepository
import com.retrosave.data.api.ApiCredentials
import com.retrosave.data.api.KtorSyncApi
import com.retrosave.data.db.AppDatabase
import com.retrosave.data.db.RoomUnitStore
import com.retrosave.data.fs.AndroidSafTreeReaderFactory
import com.retrosave.data.fs.SafVfs
import com.retrosave.data.icons.ArtworkFetcher
import com.retrosave.data.icons.IconCache
import com.retrosave.data.session.SessionNotifier
import com.retrosave.data.session.UsageSessionReader
import com.retrosave.data.settings.SettingsStore
import com.retrosave.sync.SyncService

const val APP_VERSION = "0.1.0"

/** Conteneur de dépendances explicite du POC, sans framework de DI. */
class AppContainer(
    application: Application,
) {
    val settings = SettingsStore(application)

    /** M8 §8 : vignettes tirées des sauvegardes, jamais téléchargées. */
    val icons = IconCache(application)

    val db: AppDatabase =
        Room
            .databaseBuilder(
                application,
                AppDatabase::class.java,
                "retrosave.db",
            ).addMigrations(
                AppDatabase.MIGRATION_1_2,
                AppDatabase.MIGRATION_2_3,
                AppDatabase.MIGRATION_3_4,
            ).build()

    val unitStore = RoomUnitStore(db)
    val safVfs = SafVfs(application, db.roots())
    val adapterSpecs = AdapterManifestLoader(application.assets).loadActive()
    val rootRepository = RootRepository(db.roots(), adapterSpecs.keys)
    val adapterRegistry =
        AndroidAdapterRegistry(
            contentResolver = application.contentResolver,
            roots = db.roots(),
            vfs = safVfs,
            specs = adapterSpecs,
            readerFactory = AndroidSafTreeReaderFactory(application.contentResolver),
            azaharDetector = AzaharSaveDetector(),
        )

    val api =
        KtorSyncApi(
            credentials = {
                val snapshot = settings.snapshot()
                ApiCredentials(
                    serverUrl = snapshot.serverUrl,
                    token = snapshot.token,
                    deviceId = snapshot.deviceId,
                )
            },
        )
    val syncEngine =
        SyncEngine(
            vfs = safVfs,
            api = api,
            store = unitStore,
            discovery = adapterRegistry,
            configuredRoots = adapterRegistry::configuredRoots,
            remoteTarget = adapterRegistry,
            environment =
                mapOf(
                    "os" to "android",
                    "app_version" to APP_VERSION,
                ),
        )
    val usageSessions = UsageSessionReader(application)

    /**
     * M7 §5 : les packages surveillés sont ceux des adaptateurs **configurés**,
     * pas tous ceux des manifestes. Regarder l'usage d'un émulateur dont
     * l'utilisateur n'a jamais choisi de dossier serait sans objet, et §9 exige
     * que la lecture d'usage reste bornée à ce qui sert.
     */
    suspend fun watchedPackages(): Set<String> {
        val configured = adapterRegistry.configuredRoots()
        return adapterSpecs
            .filterKeys { configured[it].orEmpty().isNotEmpty() }
            .values
            .flatMap { it.androidPackages }
            .toSet()
    }

    val sessionNotifier = SessionNotifier(application)

    /**
     * M7 §6 : avertir que le cloud est en avance, au démarrage d'une partie.
     *
     * Une seule requête légère, aucun scan local, **aucune écriture**. Une
     * panne réseau reste silencieuse : transformer une coupure de wifi en
     * alerte au moment où le joueur lance sa partie serait anxiogène pour rien.
     */
    suspend fun warnIfCloudAhead(runningPackages: Set<String>) {
        val emulators =
            adapterSpecs
                .filterValues { spec -> spec.androidPackages.any { it in runningPackages } }
                .keys
        if (emulators.isEmpty()) return
        val remote = runCatching { api.listUnits() }.getOrNull() ?: return
        val local = unitStore.listUnits()
        val behind = emulators.flatMap { unitsBehindCloud(it, remote, local) }
        sessionNotifier.warnCloudAhead(behind.distinct().sorted())
    }

    // Q45 : le chercheur de jaquettes. Il ne parle qu'au catalogue public et
    // au cache local ; il ne touche à aucune sauvegarde.
    val artworkFetcher = ArtworkFetcher(icons)
    val syncService = SyncService(syncEngine, api, settings, artworkFetcher)
}
