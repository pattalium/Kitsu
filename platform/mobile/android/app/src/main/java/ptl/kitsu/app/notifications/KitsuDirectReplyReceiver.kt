package ptl.kitsu.app.notifications

import android.annotation.SuppressLint
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.RemoteInput
import java.util.Locale
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import ptl.kitsu.app.KitsuApplication
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.navigation.IncomingTextSharePolicy
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.security.SafeLog

sealed interface DirectReplyEligibility {
    data class Ready(
        val deviceAddress: String,
        val peerKey: String,
        val text: String,
    ) : DirectReplyEligibility

    data class Rejected(val detail: String) : DirectReplyEligibility
}

/** Pure fail-closed gate used immediately before the singleton repository send. */
object DirectReplyPolicy {
    private val bluetoothAddress = Regex("^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$")

    fun evaluate(
        owner: OwnerState,
        requestedDeviceAddress: String?,
        requestedPeerKey: String?,
        rawText: CharSequence?,
    ): DirectReplyEligibility {
        val address = requestedDeviceAddress?.trim()?.uppercase(Locale.ROOT)
        if (address == null || !bluetoothAddress.matches(address)) {
            return DirectReplyEligibility.Rejected("Reply not accepted—open Kitsu to retry.")
        }
        val peerKey = requestedPeerKey?.trim()
        if (peerKey == null || !MeshPeerKeyPolicy.isCanonicalBase64Url(peerKey)) {
            return DirectReplyEligibility.Rejected("Reply not accepted—open Kitsu to retry.")
        }
        if (!owner.connection.connected || !owner.activeDeviceAddress.equals(address, ignoreCase = true)) {
            return DirectReplyEligibility.Rejected(
                "Reply not accepted—open Kitsu while the selected device is connected.",
            )
        }
        val prepared = IncomingTextSharePolicy.prepare(rawText)
            ?: return DirectReplyEligibility.Rejected("Reply not accepted—enter a message in Kitsu.")
        if (prepared.shortened) {
            return DirectReplyEligibility.Rejected(
                "Reply not accepted—open Kitsu to edit this message within the device limit.",
            )
        }
        return DirectReplyEligibility.Ready(address, peerKey, prepared.text)
    }
}

/**
 * Handles only an explicit RemoteInput PendingIntent created by this app.
 * It never connects, starts the continuity service, or implies delivery beyond the exact receipt.
 */
class KitsuDirectReplyReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_REPLY) return
        val pending = goAsync()
        val app = context.applicationContext as? KitsuApplication
        if (app == null) {
            pending.finish()
            return
        }
        val remoteText = RemoteInput.getResultsFromIntent(intent)
            ?.getCharSequence(KitsuNotificationContract.DIRECT_REPLY_RESULT_KEY)
        val requestedAddress = intent.getStringExtra(KitsuNotificationContract.EXTRA_DEVICE_ADDRESS)
        val requestedPeer = intent.getStringExtra(KitsuNotificationContract.EXTRA_PEER_KEY)
        val notificationTag = intent.getStringExtra(KitsuNotificationContract.EXTRA_NOTIFICATION_TAG)
        val notificationId = intent.getIntExtra(KitsuNotificationContract.EXTRA_NOTIFICATION_ID, -1)

        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            try {
                val repository = app.services.ownerRepository
                when (val eligibility = DirectReplyPolicy.evaluate(
                    owner = repository.state.value,
                    requestedDeviceAddress = requestedAddress,
                    requestedPeerKey = requestedPeer,
                    rawText = remoteText,
                )) {
                    is DirectReplyEligibility.Rejected -> postResult(
                        context = context,
                        tag = notificationTag,
                        id = notificationId,
                        peerKey = requestedPeer,
                        accepted = false,
                        detail = eligibility.detail,
                    )
                    is DirectReplyEligibility.Ready -> {
                        val result = runCatching {
                            repository.perform(
                                ActionCommand(
                                    kind = ActionKind.SEND_MESSAGE,
                                    clientRequestId = UUID.randomUUID().toString(),
                                    targetId = eligibility.peerKey,
                                    text = eligibility.text,
                                    messageRoute = MessageRoute.DIRECT,
                                ),
                            )
                        }
                        val receipt = result.getOrNull()
                        val accepted = receipt?.accepted == true
                        if (result.isFailure) {
                            SafeLog.warn("notification_reply", "direct_reply_not_accepted", result.exceptionOrNull())
                        }
                        postResult(
                            context = context,
                            tag = notificationTag,
                            id = notificationId,
                            peerKey = eligibility.peerKey,
                            accepted = accepted,
                            detail = if (accepted) {
                                "Reply accepted by Kitsu. Open the conversation to see its delivery status."
                            } else {
                                "Reply not accepted—open Kitsu while the selected device is connected."
                            },
                        )
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                SafeLog.warn("notification_reply", "direct_reply_failed", failure)
            } finally {
                pending.finish()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun postResult(
        context: Context,
        tag: String?,
        id: Int,
        peerKey: String?,
        accepted: Boolean,
        detail: String,
    ) {
        if (tag.isNullOrBlank() || id < 0 || peerKey == null ||
            !MeshPeerKeyPolicy.isCanonicalBase64Url(peerKey) ||
            !KitsuNotificationPermissionPolicy.canPost(context)
        ) {
            return
        }
        NotificationManagerCompat.from(context).notify(
            tag,
            id,
            KitsuNotificationRenderer.replyResult(context, peerKey, accepted, detail),
        )
    }

    companion object {
        const val ACTION_REPLY = "ptl.kitsu.app.action.DIRECT_REPLY"
    }
}
