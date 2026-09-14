package com.retrosave.data.session

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.retrosave.R
import com.retrosave.RetroSaveApp
import com.retrosave.sync.SyncTrigger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Service de premier plan **court**, le temps d'une passe déclenchée à la main
 * (AD-34).
 *
 * Il ne surveille rien et ne survit pas à la passe : il existe uniquement pour
 * qu'une synchronisation lancée depuis la tuile survive à la fermeture du volet
 * et à la mise en veille de l'écran. Le service résident, lui, reste exclu.
 *
 * Pas de travail *expedited* à la place : une unité volumineuse dépasserait le
 * quota, et la passe serait interrompue au pire moment.
 */
class SyncPassService : Service() {
    private var scope: CoroutineScope? = null
    private var pass: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(
        intent: Intent?,
        flags: Int,
        startId: Int,
    ): Int {
        if (pass != null) return START_NOT_STICKY
        startForegroundCompat()
        val running = CoroutineScope(SupervisorJob())
        scope = running
        pass =
            running.launch {
                val container = (application as RetroSaveApp).container
                // AD-28 appliquée au déclencheur : une passe qui échoue ne doit
                // pas laisser un service de premier plan derrière elle.
                runCatching { container.syncService.runPass(SyncTrigger.MANUAL) }
                stopSelf()
            }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        pass?.cancel()
        scope?.cancel()
        pass = null
        scope = null
        super.onDestroy()
    }

    private fun startForegroundCompat() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
        manager?.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                getString(R.string.sync_pass_channel_name),
                // Basse : la notification informe, elle n'interrompt pas une partie.
                NotificationManager.IMPORTANCE_LOW,
            ),
        )
        val notification: Notification =
            NotificationCompat
                .Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle(getString(R.string.sync_pass_title))
                .setOngoing(true)
                .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    companion object {
        private const val CHANNEL_ID = "retrosave-sync-pass"
        private const val NOTIFICATION_ID = 4202

        fun start(context: Context) {
            context.startForegroundService(Intent(context, SyncPassService::class.java))
        }
    }
}
