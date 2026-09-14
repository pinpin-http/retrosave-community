package com.retrosave

import android.app.Application
import com.retrosave.di.AppContainer
import com.retrosave.work.SyncWorker

/**
 * Point de création des dépendances Android qui doivent vivre aussi longtemps
 * que le processus de l'application.
 *
 * Le POC utilise volontairement une injection manuelle : aucun framework de DI
 * (Hilt/Koin) n'est ajouté tant que trois singletons simples suffisent.
 */
class RetroSaveApp : Application() {
    val container: AppContainer by lazy { AppContainer(this) }

    override fun onCreate() {
        super.onCreate()
        SyncWorker.schedule(this)
    }
}
