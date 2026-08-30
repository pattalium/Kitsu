package ptl.kitsu.app.notifications

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class KitsuNotificationSettings(
    /** Master opt-in. Android permission is checked independently before every post. */
    val alertsEnabled: Boolean = false,
    val directMessagesEnabled: Boolean = true,
    val petUpdatesEnabled: Boolean = true,
    /** Process-scoped user intent; reset on every process start and never auto-started. */
    val connectionContinuityEnabled: Boolean = false,
)

interface KitsuNotificationSettingsStore {
    val settings: StateFlow<KitsuNotificationSettings>
    fun setAlertsEnabled(enabled: Boolean)
    fun setDirectMessagesEnabled(enabled: Boolean)
    fun setPetUpdatesEnabled(enabled: Boolean)
    fun setConnectionContinuityEnabled(enabled: Boolean)
    fun resetConnectionContinuityForProcessStart()
}

class AndroidKitsuNotificationSettingsStore(context: Context) : KitsuNotificationSettingsStore {
    private val preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
    private val mutableSettings = MutableStateFlow(read())
    override val settings: StateFlow<KitsuNotificationSettings> = mutableSettings.asStateFlow()

    @Synchronized
    override fun setAlertsEnabled(enabled: Boolean) = update {
        it.copy(
            alertsEnabled = enabled,
            connectionContinuityEnabled = it.connectionContinuityEnabled && enabled,
        )
    }

    @Synchronized
    override fun setDirectMessagesEnabled(enabled: Boolean) = update {
        it.copy(directMessagesEnabled = enabled)
    }

    @Synchronized
    override fun setPetUpdatesEnabled(enabled: Boolean) = update {
        it.copy(petUpdatesEnabled = enabled)
    }

    @Synchronized
    override fun setConnectionContinuityEnabled(enabled: Boolean) {
        mutableSettings.value = mutableSettings.value.copy(
            connectionContinuityEnabled = enabled && mutableSettings.value.alertsEnabled,
        )
    }

    @Synchronized
    override fun resetConnectionContinuityForProcessStart() {
        mutableSettings.value = mutableSettings.value.copy(connectionContinuityEnabled = false)
    }

    private fun read(): KitsuNotificationSettings = KitsuNotificationSettings(
        alertsEnabled = preferences.getBoolean(ALERTS, false),
        directMessagesEnabled = preferences.getBoolean(DIRECT_MESSAGES, true),
        petUpdatesEnabled = preferences.getBoolean(PET_UPDATES, true),
        // Never trust a persisted foreground-service bit after a new process starts.
        connectionContinuityEnabled = false,
    )

    @SuppressLint("UseKtx") // Keep the synchronous commit result: failed opt-in writes must not look enabled.
    private fun update(transform: (KitsuNotificationSettings) -> KitsuNotificationSettings) {
        val updated = transform(mutableSettings.value)
        val written = preferences.edit()
            .putBoolean(ALERTS, updated.alertsEnabled)
            .putBoolean(DIRECT_MESSAGES, updated.directMessagesEnabled)
            .putBoolean(PET_UPDATES, updated.petUpdatesEnabled)
            .commit()
        if (written) mutableSettings.value = updated
    }

    private companion object {
        const val PREFERENCES = "kitsu_notification_settings_v1"
        const val ALERTS = "alerts_enabled"
        const val DIRECT_MESSAGES = "direct_messages_enabled"
        const val PET_UPDATES = "pet_updates_enabled"
    }
}

object KitsuNotificationPermissionPolicy {
    fun canPost(context: Context): Boolean =
        NotificationManagerCompat.from(context).areNotificationsEnabled() &&
            (Build.VERSION.SDK_INT < 33 || ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.POST_NOTIFICATIONS,
            ) == PackageManager.PERMISSION_GRANTED)
}
