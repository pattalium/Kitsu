package ptl.kitsu.app

import android.Manifest
import android.app.Notification
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.navigation.AppLaunchIntentPolicy
import ptl.kitsu.app.notifications.AndroidKitsuNotificationSettingsStore
import ptl.kitsu.app.notifications.KitsuConnectedDeviceService
import ptl.kitsu.app.notifications.KitsuDirectReplyReceiver
import ptl.kitsu.app.notifications.KitsuNotificationContract
import ptl.kitsu.app.notifications.KitsuNotificationRenderer
import ptl.kitsu.app.notifications.PetNotificationEvent
import ptl.kitsu.app.notifications.PetNotificationKind

@RunWith(AndroidJUnit4::class)
class KitsuNotificationsInstrumentationTest {
    private val context
        get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun directNotificationIsPrivateHasRemoteReplyAndNeverPlacesTextInItsIntent() {
        KitsuNotificationRenderer.ensureChannels(context)
        val posted = KitsuNotificationRenderer.directMessage(
            context = context,
            deviceAddress = "02:00:00:00:00:32",
            message = Message(
                id = "42",
                cursor = "42",
                direction = "inbound",
                peerId = peer,
                text = "Meet at the quiet trail.",
                state = "received",
                occurredAt = 1_787_054_400,
                journalSession = "boot",
                senderName = "Copper Fox",
                route = "direct",
            ),
        )

        assertEquals(NotificationCompat.VISIBILITY_PRIVATE, posted.notification.visibility)
        assertEquals("Copper Fox", posted.notification.extras.getCharSequence(Notification.EXTRA_TITLE))
        assertEquals("Meet at the quiet trail.", posted.notification.extras.getCharSequence(Notification.EXTRA_TEXT))
        assertNotNull(posted.notification.publicVersion)
        val publicVersion = requireNotNull(posted.notification.publicVersion)
        assertEquals(
            context.getString(R.string.notification_private_message_title),
            publicVersion.extras.getCharSequence(Notification.EXTRA_TITLE),
        )
        assertEquals(
            context.getString(R.string.notification_unlock_to_view),
            publicVersion.extras.getCharSequence(Notification.EXTRA_TEXT),
        )
        assertFalse(publicVersion.extras.toString().contains("quiet trail"))

        val action = posted.notification.actions.single()
        assertEquals(context.getString(R.string.notification_reply_action), action.title)
        assertEquals(
            KitsuNotificationContract.DIRECT_REPLY_RESULT_KEY,
            action.remoteInputs.single().resultKey,
        )
        if (Build.VERSION.SDK_INT >= 31) assertFalse(action.actionIntent.isImmutable)

        val replyIntent = KitsuNotificationContract.directReplyBroadcastIntent(
            context,
            "02:00:00:00:00:32",
            peer,
            "42",
            posted.tag,
            posted.id,
        )
        assertEquals(ComponentName(context, KitsuDirectReplyReceiver::class.java), replyIntent.component)
        assertEquals(KitsuDirectReplyReceiver.ACTION_REPLY, replyIntent.action)
        assertFalse(replyIntent.hasExtra(Intent.EXTRA_TEXT))
        assertFalse(replyIntent.extras.orEmpty().toString().contains("quiet trail"))
    }

    @Test fun threadLaunchIntentsAreExplicitImmutableAndDistinctPerConversation() {
        val otherPeer = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"
        val first = KitsuNotificationContract.directThreadLaunchIntent(context, peer)
        val second = KitsuNotificationContract.directThreadLaunchIntent(context, otherPeer)

        assertEquals(ComponentName(context, MainActivity::class.java), first.component)
        assertEquals(AppLaunchIntentPolicy.ACTION_OPEN_MESSAGES, first.action)
        assertEquals("direct:$peer", first.getStringExtra(AppLaunchIntentPolicy.EXTRA_THREAD_KEY))
        assertFalse(first.hasExtra(Intent.EXTRA_TEXT))
        assertNotEquals(first.data, second.data)
        val pending = KitsuNotificationContract.directThreadIntent(context, peer)
        if (Build.VERSION.SDK_INT >= 31) assertTrue(pending.isImmutable)
    }

    @Test fun petAndContinuityNotificationsExposeOnlyGenericLockscreenVersions() {
        val pet = KitsuNotificationRenderer.petUpdate(
            context,
            "02:00:00:00:00:32",
            PetNotificationEvent(
                PetNotificationKind.WALK,
                "Shade needs help on the walk",
                "Open Kitsu to choose what happens next.",
                "walk:8:AWAITING_RESCUE:NONE",
            ),
        ).notification
        assertEquals(NotificationCompat.VISIBILITY_PRIVATE, pet.visibility)
        assertEquals("Shade needs help on the walk", pet.extras.getCharSequence(Notification.EXTRA_TITLE))
        val petPublic = requireNotNull(pet.publicVersion)
        assertEquals(
            context.getString(R.string.notification_private_pet_title),
            petPublic.extras.getCharSequence(Notification.EXTRA_TITLE),
        )
        assertFalse(petPublic.extras.toString().contains("Shade"))

        val continuity = KitsuNotificationRenderer.connectionContinuity(context, "Pocket Kitsu")
        assertEquals(NotificationCompat.VISIBILITY_PRIVATE, continuity.visibility)
        assertTrue(
            continuity.extras.getCharSequence(Notification.EXTRA_TITLE).toString().contains("Pocket Kitsu"),
        )
        assertFalse(requireNotNull(continuity.publicVersion).extras.toString().contains("Pocket Kitsu"))
    }

    @Test fun channelsManifestAndProcessScopedOptInAreFailClosed() {
        KitsuNotificationRenderer.ensureChannels(context)
        val manager = context.getSystemService(NotificationManager::class.java)
        assertEquals(
            NotificationManager.IMPORTANCE_HIGH,
            manager.getNotificationChannel(KitsuNotificationContract.DIRECT_MESSAGE_CHANNEL).importance,
        )
        assertEquals(
            NotificationManager.IMPORTANCE_DEFAULT,
            manager.getNotificationChannel(KitsuNotificationContract.PET_UPDATES_CHANNEL).importance,
        )
        assertEquals(
            NotificationManager.IMPORTANCE_LOW,
            manager.getNotificationChannel(KitsuNotificationContract.CONNECTION_CHANNEL).importance,
        )

        @Suppress("DEPRECATION")
        val receiver = context.packageManager.getReceiverInfo(
            ComponentName(context, KitsuDirectReplyReceiver::class.java),
            0,
        )
        assertFalse(receiver.exported)
        @Suppress("DEPRECATION")
        val service = context.packageManager.getServiceInfo(
            ComponentName(context, KitsuConnectedDeviceService::class.java),
            0,
        )
        assertFalse(service.exported)
        assertEquals(
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            service.foregroundServiceType,
        )

        @Suppress("DEPRECATION")
        val packageInfo = context.packageManager.getPackageInfo(
            context.packageName,
            PackageManager.GET_PERMISSIONS,
        )
        val requested = packageInfo.requestedPermissions.orEmpty().toSet()
        assertTrue(Manifest.permission.POST_NOTIFICATIONS in requested)
        assertTrue(Manifest.permission.FOREGROUND_SERVICE in requested)
        assertTrue(Manifest.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE in requested)
        assertFalse(Manifest.permission.RECEIVE_BOOT_COMPLETED in requested)
        @Suppress("DEPRECATION")
        assertTrue(
            context.packageManager.queryBroadcastReceivers(
                Intent(Intent.ACTION_BOOT_COMPLETED).setPackage(context.packageName),
                PackageManager.MATCH_ALL,
            ).isEmpty(),
        )

        val settings = AndroidKitsuNotificationSettingsStore(context)
        try {
            settings.setAlertsEnabled(true)
            settings.setConnectionContinuityEnabled(true)
            assertTrue(settings.settings.value.connectionContinuityEnabled)
            val recreated = AndroidKitsuNotificationSettingsStore(context)
            assertTrue(recreated.settings.value.alertsEnabled)
            assertFalse(recreated.settings.value.connectionContinuityEnabled)
            settings.setAlertsEnabled(false)
            assertFalse(settings.settings.value.connectionContinuityEnabled)
        } finally {
            settings.setConnectionContinuityEnabled(false)
            settings.setAlertsEnabled(false)
            settings.setDirectMessagesEnabled(true)
            settings.setPetUpdatesEnabled(true)
        }
    }

    private fun android.os.Bundle?.orEmpty(): android.os.Bundle = this ?: android.os.Bundle.EMPTY
}
