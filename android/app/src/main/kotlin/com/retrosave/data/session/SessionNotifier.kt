package com.retrosave.data.session

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.retrosave.MainActivity
import com.retrosave.R

/**
 * Avertissement « une sauvegarde plus récente dort dans le cloud » (M7 §6).
 *
 * La notification **ouvre l'application, elle ne déclenche rien**. Le cas 6 du
 * banc interdit d'écrire pendant qu'un émulateur tourne, et un raccourci
 * « appliquer » ici serait précisément la manière de le violer sans s'en
 * apercevoir — au moment le plus dangereux, puisque le jeu a le fichier ouvert.
 */
class SessionNotifier(
    private val context: Context,
) {
    fun warnCloudAhead(unitKeys: List<String>) {
        if (unitKeys.isEmpty() || !canNotify()) return
        ensureChannel()

        val open =
            PendingIntent.getActivity(
                context,
                0,
                Intent(context, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )

        val body =
            context.resources.getQuantityString(
                R.plurals.session_cloud_ahead_body,
                unitKeys.size,
                unitKeys.size,
            )
        val notification =
            NotificationCompat
                .Builder(context, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setContentTitle(context.getString(R.string.session_cloud_ahead_title))
                .setContentText(body)
                .setStyle(NotificationCompat.BigTextStyle().bigText(body))
                .setPriority(NotificationCompat.PRIORITY_DEFAULT)
                .setContentIntent(open)
                .setAutoCancel(true)
                .build()

        // La permission a pu être retirée entre le contrôle et l'envoi ; NotificationManagerCompat
        // lève dans ce cas, et rien de tout ceci ne justifie de faire tomber une passe.
        runCatching { NotificationManagerCompat.from(context).notify(NOTIFICATION_ID, notification) }
    }

    private fun canNotify(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return true
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.POST_NOTIFICATIONS,
        ) == PackageManager.PERMISSION_GRANTED
    }

    private fun ensureChannel() {
        val manager =
            context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
                ?: return
        // Créer un canal existant est sans effet : pas besoin de mémoriser
        // qu'on l'a déjà fait, et un canal supprimé par l'utilisateur revient.
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                context.getString(R.string.session_channel_name),
                NotificationManager.IMPORTANCE_DEFAULT,
            ).apply {
                description = context.getString(R.string.session_channel_description)
            },
        )
    }

    private companion object {
        const val CHANNEL_ID = "retrosave-session"
        const val NOTIFICATION_ID = 4201
    }
}
