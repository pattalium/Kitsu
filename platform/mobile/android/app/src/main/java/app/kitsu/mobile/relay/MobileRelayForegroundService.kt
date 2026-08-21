package app.kitsu.mobile.relay

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import app.kitsu.mobile.KitsuApplication
import app.kitsu.mobile.R
import app.kitsu.mobile.security.SafeLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

class MobileRelayForegroundService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var relayJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        val notifications = getSystemService(NotificationManager::class.java)
        notifications.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                "Kitsu mobile relay",
                NotificationManager.IMPORTANCE_LOW,
            ).apply {
                description = "Keeps selected paired Kitsu devices connected to the owner gateway"
            },
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        try {
            startForeground(
                NOTIFICATION_ID,
                NotificationCompat.Builder(this, CHANNEL_ID)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle("Kitsu mobile relay")
                    .setContentText("Relaying for up to three selected paired devices")
                    .setOngoing(true)
                    .setCategory(NotificationCompat.CATEGORY_SERVICE)
                    .build(),
            )
        } catch (failure: SecurityException) {
            SafeLog.warn("mobile_relay", "foreground_permission_required", failure)
            stopSelf()
            return START_NOT_STICKY
        }
        if (relayJob?.isActive != true) {
            val controller = (application as KitsuApplication).services.mobileRelayController
            relayJob = scope.launch {
                try {
                    controller.runForegroundService()
                } finally {
                    stopSelf()
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        relayJob?.cancel()
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private companion object {
        const val CHANNEL_ID = "kitsu_mobile_relay"
        const val NOTIFICATION_ID = 41
    }
}
