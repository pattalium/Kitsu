package ptl.kitsu.app.notifications

import android.app.Service
import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import ptl.kitsu.app.KitsuApplication
import ptl.kitsu.app.security.SafeLog

/**
 * Keeps only an already-established singleton repository/GATT process alive.
 * This service has no connect path, is never sticky, and has no boot/background entry point.
 */
class KitsuConnectedDeviceService : Service() {
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var observation: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action != ACTION_USER_START) {
            stopSelf(startId)
            return START_NOT_STICKY
        }
        val app = application as? KitsuApplication ?: run {
            stopSelf(startId)
            return START_NOT_STICKY
        }
        val settingsStore = app.services.notificationSettingsStore
        val owner = app.services.ownerRepository.state.value
        val settings = settingsStore.settings.value
        if (!settings.alertsEnabled || !settings.connectionContinuityEnabled ||
            !owner.connection.connected
        ) {
            settingsStore.setConnectionContinuityEnabled(false)
            stopSelf(startId)
            return START_NOT_STICKY
        }

        KitsuNotificationRenderer.ensureChannels(this)
        if (!enterForeground(owner.activeDeviceDisplayName())) {
            settingsStore.setConnectionContinuityEnabled(false)
            stopSelf(startId)
            return START_NOT_STICKY
        }
        observation?.cancel()
        observation = serviceScope.launch {
            combine(
                app.services.ownerRepository.state,
                settingsStore.settings,
            ) { state, currentSettings -> state to currentSettings }
                .distinctUntilChanged { old, new ->
                    old.first.connection.connected == new.first.connection.connected &&
                        old.first.activeDeviceAddress == new.first.activeDeviceAddress &&
                        old.first.activeDeviceDisplayName() == new.first.activeDeviceDisplayName() &&
                        old.second.alertsEnabled == new.second.alertsEnabled &&
                        old.second.connectionContinuityEnabled ==
                        new.second.connectionContinuityEnabled
                }
                .collect { (state, currentSettings) ->
                    if (!currentSettings.alertsEnabled ||
                        !currentSettings.connectionContinuityEnabled ||
                        !state.connection.connected
                    ) {
                        settingsStore.setConnectionContinuityEnabled(false)
                        stopSelf()
                        return@collect
                    }
                    enterForeground(state.activeDeviceDisplayName())
                }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        observation?.cancel()
        serviceScope.cancel()
        (application as? KitsuApplication)?.services?.notificationSettingsStore
            ?.setConnectionContinuityEnabled(false)
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }

    private fun ptl.kitsu.app.repository.OwnerState.activeDeviceDisplayName(): String? =
        savedKitsu.firstOrNull { saved ->
            saved.deviceAddress.equals(activeDeviceAddress, ignoreCase = true)
        }?.displayName ?: status?.companionName

    private fun enterForeground(displayName: String?): Boolean = try {
        val notification = KitsuNotificationRenderer.connectionContinuity(this, displayName)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(
                KitsuNotificationContract.CONNECTION_NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } else {
            startForeground(KitsuNotificationContract.CONNECTION_NOTIFICATION_ID, notification)
        }
        true
    } catch (failure: RuntimeException) {
        SafeLog.warn("connection_continuity", "foreground_service_not_allowed", failure)
        false
    }

    companion object {
        internal const val ACTION_USER_START = "ptl.kitsu.app.action.START_CONNECTED_DEVICE_SERVICE"

        /** Call only from the foreground settings interaction after permission and connection checks. */
        fun startFromVisibleAction(context: Context) {
            ContextCompat.startForegroundService(
                context,
                Intent(context, KitsuConnectedDeviceService::class.java).setAction(ACTION_USER_START),
            )
        }

        @SuppressLint("ImplicitSamInstance")
        fun stop(context: Context) {
            context.stopService(Intent(context, KitsuConnectedDeviceService::class.java))
        }
    }
}
