package ptl.kitsu.app.widget

import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.EncryptedBoundedCache
import ptl.kitsu.app.cache.OwnerCacheBindingPolicy
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.security.AndroidKeystoreCredentialStore
import ptl.kitsu.app.security.SafeLog

class KitsuStatusWidgetProvider : AppWidgetProvider() {
    override fun onUpdate(
        context: Context,
        appWidgetManager: AppWidgetManager,
        appWidgetIds: IntArray,
    ) {
        if (appWidgetIds.isEmpty()) return
        val pendingResult = goAsync()
        updateScope.launch {
            try {
                val source = KitsuStatusWidgetSnapshotLoader.load(context.applicationContext)
                KitsuStatusWidgetUpdater.update(
                    context = context.applicationContext,
                    manager = appWidgetManager,
                    appWidgetIds = appWidgetIds,
                    source = source,
                )
            } catch (failure: Throwable) {
                SafeLog.warn("status_widget", "status_widget_update_failed", failure)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private companion object {
        val updateScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    }
}

object KitsuStatusWidgetSnapshotLoader {
    suspend fun load(context: Context): KitsuStatusWidgetSource = runCatching {
        val activeAddress = AndroidKeystoreCredentialStore(context)
            .bondedCompanion()
            ?.deviceAddress
        val snapshot = OwnerCacheBindingPolicy.restore(
            snapshot = EncryptedBoundedCache(context).read(),
            activeDeviceAddress = activeAddress,
        )
        fromCache(snapshot)
    }.getOrElse { failure ->
        SafeLog.warn("status_widget", "status_widget_snapshot_failed", failure)
        fromCache(null)
    }

    fun fromCache(snapshot: CacheSnapshot?): KitsuStatusWidgetSource = KitsuStatusWidgetSource(
        status = snapshot?.status,
        connected = false,
        snapshotAtEpochSeconds = snapshot?.writtenAt,
    )
}

object KitsuStatusWidgetUpdater {
    fun sourceFromOwner(
        owner: OwnerState,
        companionProfile: CompanionProfile? = null,
        focusSession: FocusSessionState? = null,
        walkAdventure: WalkAdventureState? = null,
    ): KitsuStatusWidgetSource = KitsuStatusWidgetSource(
        status = owner.status,
        connected = owner.connection.connected,
        snapshotAtEpochSeconds = owner.status?.updatedAt,
        companionProfile = companionProfile ?: owner.companionProfile,
        companionCheckIn = (companionProfile ?: owner.companionProfile)?.checkIn,
        focusSession = focusSession ?: owner.focusState,
        walkAdventure = walkAdventure ?: owner.walkState,
    )

    fun updateAll(context: Context, source: KitsuStatusWidgetSource) {
        val manager = AppWidgetManager.getInstance(context)
        val ids = manager.getAppWidgetIds(ComponentName(context, KitsuStatusWidgetProvider::class.java))
        if (ids.isEmpty()) return
        update(context, manager, ids, source)
    }

    internal fun update(
        context: Context,
        manager: AppWidgetManager,
        appWidgetIds: IntArray,
        source: KitsuStatusWidgetSource,
        nowEpochSeconds: Long = System.currentTimeMillis() / 1_000L,
    ) {
        if (appWidgetIds.isEmpty()) return
        val presentation = KitsuStatusWidgetPresentationPolicy.present(
            source = source,
            nowEpochSeconds = nowEpochSeconds,
        )
        val views = KitsuStatusWidgetRenderer.render(context, presentation)
        appWidgetIds.forEach { appWidgetId -> manager.updateAppWidget(appWidgetId, views) }
    }
}
