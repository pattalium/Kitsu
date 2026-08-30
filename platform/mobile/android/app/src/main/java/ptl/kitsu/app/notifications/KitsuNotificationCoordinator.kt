package ptl.kitsu.app.notifications

import android.app.Application
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import ptl.kitsu.app.repository.OwnerRepository
import ptl.kitsu.app.ui.ModerationPreferences

/**
 * App-lifetime observer of the one singleton repository. Cursors advance even while alerts are
 * disabled, so enabling an opt-in never replays cached history as new activity.
 */
class KitsuNotificationCoordinator(
    private val application: Application,
    repository: OwnerRepository,
    settingsStore: KitsuNotificationSettingsStore,
    scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    private var directCursor = DirectMessageNotificationCursor()
    private var petCursor = PetNotificationCursor()
    private val moderationPreferences = ModerationPreferences(application)

    @Suppress("unused")
    private val observation = scope.launch {
        combine(repository.state, settingsStore.settings) { owner, settings -> owner to settings }
            .collect { (owner, settings) ->
                val blocked = moderationPreferences.blockedPeerIds()
                val directEvaluation = DirectMessageNotificationPolicy.evaluate(
                    cursor = directCursor,
                    snapshot = DirectMessageNotificationSnapshot(
                        deviceAddress = owner.activeDeviceAddress,
                        connected = owner.connection.connected,
                        journalSession = owner.messageJournalSession,
                        messages = owner.messages,
                        blockedPeerIds = blocked,
                    ),
                )
                directCursor = directEvaluation.cursor

                val petEvaluation = PetNotificationPolicy.evaluate(
                    cursor = petCursor,
                    snapshot = PetNotificationSnapshot(
                        deviceAddress = owner.activeDeviceAddress,
                        connected = owner.connection.connected,
                        profile = owner.companionProfile,
                        focus = owner.focusState,
                        walk = owner.walkState,
                        liveStateReady = owner.companionProfileSupported ||
                            owner.focusSupported || owner.walkSupported,
                    ),
                )
                petCursor = petEvaluation.cursor

                if (!settings.alertsEnabled || !KitsuNotificationPermissionPolicy.canPost(application)) {
                    return@collect
                }
                KitsuNotificationRenderer.ensureChannels(application)
                val address = owner.activeDeviceAddress ?: return@collect
                if (settings.directMessagesEnabled) {
                    directEvaluation.newMessages.forEach { message ->
                        KitsuNotificationRenderer.post(
                            application,
                            KitsuNotificationRenderer.directMessage(application, address, message),
                        )
                    }
                }
                if (settings.petUpdatesEnabled) {
                    petEvaluation.events.forEach { event ->
                        KitsuNotificationRenderer.post(
                            application,
                            KitsuNotificationRenderer.petUpdate(application, address, event),
                        )
                    }
                }
            }
    }
}
