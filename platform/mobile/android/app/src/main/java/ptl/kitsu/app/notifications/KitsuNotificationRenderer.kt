package ptl.kitsu.app.notifications

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.RemoteInput
import java.util.Locale
import ptl.kitsu.app.MainActivity
import ptl.kitsu.app.R
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.navigation.AppLaunchIntentPolicy

data class KitsuPostedNotification(
    val tag: String,
    val id: Int,
    val notification: Notification,
)

/** Android-only intent identity and extras shared by the renderer and the private reply receiver. */
object KitsuNotificationContract {
    const val DIRECT_MESSAGE_CHANNEL = "kitsu_direct_messages_v1"
    const val PET_UPDATES_CHANNEL = "kitsu_pet_updates_v1"
    const val CONNECTION_CHANNEL = "kitsu_connection_continuity_v1"
    const val DIRECT_REPLY_RESULT_KEY = "kitsu_direct_reply_text"

    const val EXTRA_DEVICE_ADDRESS = "ptl.kitsu.app.extra.NOTIFICATION_DEVICE_ADDRESS"
    const val EXTRA_PEER_KEY = "ptl.kitsu.app.extra.NOTIFICATION_PEER_KEY"
    const val EXTRA_MESSAGE_ID = "ptl.kitsu.app.extra.NOTIFICATION_MESSAGE_ID"
    const val EXTRA_NOTIFICATION_TAG = "ptl.kitsu.app.extra.NOTIFICATION_TAG"
    const val EXTRA_NOTIFICATION_ID = "ptl.kitsu.app.extra.NOTIFICATION_ID"

    const val CONNECTION_NOTIFICATION_ID = 0x4b17

    internal fun stableId(identity: String): Int = identity.hashCode() and Int.MAX_VALUE

    internal fun directThreadLaunchIntent(context: Context, peerKey: String): Intent {
        val threadKey = "direct:$peerKey"
        return Intent(context, MainActivity::class.java).apply {
            action = AppLaunchIntentPolicy.ACTION_OPEN_MESSAGES
            data = Uri.Builder()
                .scheme("kitsu")
                .authority("messages")
                .appendPath("direct")
                .appendPath(peerKey)
                .build()
            putExtra(AppLaunchIntentPolicy.EXTRA_THREAD_KEY, threadKey)
            addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
    }

    internal fun directThreadIntent(context: Context, peerKey: String): PendingIntent {
        val threadKey = "direct:$peerKey"
        return PendingIntent.getActivity(
            context,
            stableId("thread\u0000$threadKey"),
            directThreadLaunchIntent(context, peerKey),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    internal fun homeIntent(context: Context, identity: String): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            action = AppLaunchIntentPolicy.ACTION_OPEN_HOME
            data = Uri.Builder()
                .scheme("kitsu")
                .authority("home")
                .appendPath(identity)
                .build()
            addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        }
        return PendingIntent.getActivity(
            context,
            stableId("home\u0000$identity"),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    internal fun directReplyBroadcastIntent(
        context: Context,
        deviceAddress: String,
        peerKey: String,
        messageId: String,
        notificationTag: String,
        notificationId: Int,
    ): Intent {
        return Intent(context, KitsuDirectReplyReceiver::class.java).apply {
            action = KitsuDirectReplyReceiver.ACTION_REPLY
            data = Uri.Builder()
                .scheme("kitsu")
                .authority("reply")
                .appendPath(deviceAddress)
                .appendPath(peerKey)
                .appendPath(messageId)
                .build()
            putExtra(EXTRA_DEVICE_ADDRESS, deviceAddress)
            putExtra(EXTRA_PEER_KEY, peerKey)
            putExtra(EXTRA_MESSAGE_ID, messageId)
            putExtra(EXTRA_NOTIFICATION_TAG, notificationTag)
            putExtra(EXTRA_NOTIFICATION_ID, notificationId)
        }
    }

    internal fun directReplyIntent(
        context: Context,
        deviceAddress: String,
        peerKey: String,
        messageId: String,
        notificationTag: String,
        notificationId: Int,
    ): PendingIntent {
        val identity = "$deviceAddress\u0000$peerKey\u0000$messageId"
        return PendingIntent.getBroadcast(
            context,
            stableId("reply\u0000$identity"),
            directReplyBroadcastIntent(
                context,
                deviceAddress,
                peerKey,
                messageId,
                notificationTag,
                notificationId,
            ),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
        )
    }
}

object KitsuNotificationRenderer {
    fun ensureChannels(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return
        val accent = Color.rgb(240, 154, 104)
        manager.createNotificationChannels(
            listOf(
                NotificationChannel(
                    KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL,
                    context.getString(R.string.notification_channel_direct_messages),
                    NotificationManager.IMPORTANCE_HIGH,
                ).apply {
                    description = context.getString(R.string.notification_channel_direct_messages_description)
                    enableVibration(true)
                    lightColor = accent
                },
                NotificationChannel(
                    KitsuNotificationContract.PET_UPDATES_CHANNEL,
                    context.getString(R.string.notification_channel_pet_updates),
                    NotificationManager.IMPORTANCE_DEFAULT,
                ).apply {
                    description = context.getString(R.string.notification_channel_pet_updates_description)
                    lightColor = accent
                },
                NotificationChannel(
                    KitsuNotificationContract.CONNECTION_CHANNEL,
                    context.getString(R.string.notification_channel_connection),
                    NotificationManager.IMPORTANCE_LOW,
                ).apply {
                    description = context.getString(R.string.notification_channel_connection_description)
                    setShowBadge(false)
                    lightColor = accent
                },
            ),
        )
    }

    fun directMessage(
        context: Context,
        deviceAddress: String,
        message: Message,
    ): KitsuPostedNotification {
        val peerKey = requireNotNull(message.peerId)
        val sender = message.senderName.trim().ifBlank { "Direct message" }
        val identity = "$deviceAddress\u0000$peerKey\u0000${message.id}"
        val tag = "kitsu.dm.$deviceAddress.$peerKey.${message.id}"
        val id = KitsuNotificationContract.stableId("dm\u0000$identity")
        val contentIntent = KitsuNotificationContract.directThreadIntent(context, peerKey)
        val publicVersion = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(context.getString(R.string.notification_private_message_title))
            .setContentText(context.getString(R.string.notification_unlock_to_view))
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .build()
        val replyInput = RemoteInput.Builder(KitsuNotificationContract.DIRECT_REPLY_RESULT_KEY)
            .setLabel(context.getString(R.string.notification_reply_hint, sender))
            .build()
        val replyAction = NotificationCompat.Action.Builder(
            R.drawable.kitsu_app_icon_monochrome,
            context.getString(R.string.notification_reply_action),
            KitsuNotificationContract.directReplyIntent(
                context = context,
                deviceAddress = deviceAddress.uppercase(Locale.ROOT),
                peerKey = peerKey,
                messageId = message.id,
                notificationTag = tag,
                notificationId = id,
            ),
        )
            .addRemoteInput(replyInput)
            .setAllowGeneratedReplies(true)
            .setSemanticAction(NotificationCompat.Action.SEMANTIC_ACTION_REPLY)
            .build()
        val whenMillis = message.occurredAt.coerceIn(0L, Long.MAX_VALUE / 1_000L) * 1_000L
        val notification = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(sender)
            .setContentText(message.text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(message.text))
            .setWhen(whenMillis)
            .setShowWhen(whenMillis > 0L)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion)
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .addAction(replyAction)
            .build()
        return KitsuPostedNotification(tag, id, notification)
    }

    fun petUpdate(
        context: Context,
        deviceAddress: String,
        event: PetNotificationEvent,
    ): KitsuPostedNotification {
        val identity = "$deviceAddress\u0000${event.kind.name}\u0000${event.fingerprint}"
        val contentIntent = KitsuNotificationContract.homeIntent(
            context,
            "pet-${KitsuNotificationContract.stableId(identity)}",
        )
        val publicVersion = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.PET_UPDATES_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(context.getString(R.string.notification_private_pet_title))
            .setContentText(context.getString(R.string.notification_unlock_to_view))
            .setCategory(NotificationCompat.CATEGORY_REMINDER)
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .build()
        val notification = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.PET_UPDATES_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(event.title)
            .setContentText(event.text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(event.text))
            .setCategory(NotificationCompat.CATEGORY_REMINDER)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion)
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .build()
        return KitsuPostedNotification(
            tag = "kitsu.pet.${KitsuNotificationContract.stableId(identity)}",
            id = KitsuNotificationContract.stableId("pet\u0000$identity"),
            notification = notification,
        )
    }

    fun connectionContinuity(context: Context, displayName: String?): Notification {
        val name = displayName?.trim().orEmpty().ifBlank { "Kitsu" }
        val contentIntent = KitsuNotificationContract.homeIntent(context, "connection")
        val publicVersion = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.CONNECTION_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(context.getString(R.string.notification_connection_public_title))
            .setContentText(context.getString(R.string.notification_connection_public_text))
            .setOngoing(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(contentIntent)
            .build()
        return NotificationCompat.Builder(context, KitsuNotificationContract.CONNECTION_CHANNEL)
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(context.getString(R.string.notification_connection_title, name))
            .setContentText(context.getString(R.string.notification_connection_text))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion)
            .setContentIntent(contentIntent)
            .build()
    }

    fun replyResult(
        context: Context,
        peerKey: String,
        accepted: Boolean,
        detail: String,
    ): Notification {
        val contentIntent = KitsuNotificationContract.directThreadIntent(context, peerKey)
        val publicVersion = NotificationCompat.Builder(
            context,
            KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL,
        )
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(context.getString(R.string.notification_reply_result_public))
            .setContentText(context.getString(R.string.notification_unlock_to_view))
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .build()
        return NotificationCompat.Builder(context, KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL)
            .setSmallIcon(R.drawable.kitsu_app_icon_monochrome)
            .setContentTitle(
                context.getString(
                    if (accepted) R.string.notification_reply_accepted_title
                    else R.string.notification_reply_rejected_title,
                ),
            )
            .setContentText(detail)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(publicVersion)
            .setContentIntent(contentIntent)
            .setAutoCancel(true)
            .build()
    }

    @SuppressLint("MissingPermission")
    fun post(context: Context, posted: KitsuPostedNotification) {
        NotificationManagerCompat.from(context).notify(posted.tag, posted.id, posted.notification)
    }
}
