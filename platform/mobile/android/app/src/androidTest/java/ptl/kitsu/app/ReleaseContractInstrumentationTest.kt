package ptl.kitsu.app

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.drawable.AdaptiveIconDrawable
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import ptl.kitsu.app.connection.AndroidReconnectSuppressionStore
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterUnlockCode
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.security.AndroidKeystoreEncounterCodeVault
import ptl.kitsu.app.security.AndroidKeystoreMessageDraftStore
import ptl.kitsu.app.security.MessageDraftRecord
import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.ui.MeshUserPolicy
import ptl.kitsu.app.ui.ModerationPreferences
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import kotlinx.coroutines.runBlocking
import java.net.URI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ReleaseContractInstrumentationTest {
    @Test fun launcherLoadsAsAnAdaptiveIconOnEverySupportedApi() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val icon = context.packageManager.getApplicationIcon(context.applicationInfo)
        assertTrue(icon is AdaptiveIconDrawable)
        icon as AdaptiveIconDrawable
        assertNotNull(icon.background)
        assertNotNull(icon.foreground)
        if (Build.VERSION.SDK_INT >= 33) assertNotNull(icon.monochrome)
    }

    @Test fun productionIdentityAndProvenanceAreExact() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        assertEquals(AUTHORITATIVE_APPLICATION_ID, context.packageName.removeSuffix(".debug"))
        assertEquals(context.packageName, BuildConfig.APPLICATION_ID)
        assertTrue(
            context.packageName == AUTHORITATIVE_APPLICATION_ID ||
                context.packageName == "$AUTHORITATIVE_APPLICATION_ID.debug",
        )
        assertEquals(36, context.applicationInfo.targetSdkVersion)
        val packageInfo = context.packageManager.getPackageInfo(context.packageName, 0)
        assertEquals(packageInfo.longVersionCode.toInt(), BuildConfig.VERSION_CODE)
        assertEquals(packageInfo.versionName, BuildConfig.VERSION_NAME)
        assertEquals(35, BuildConfig.VERSION_CODE)
        assertEquals(
            if (context.packageName.endsWith(".debug")) "2.3.1-debug" else "2.3.1",
            BuildConfig.VERSION_NAME,
        )
        assertTrue(
            BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256 == "unbound" ||
                Regex("^[0-9a-f]{64}$").matches(BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256),
        )
        val applicationInfo = context.packageManager.getApplicationInfo(
            context.packageName,
            PackageManager.GET_META_DATA,
        )
        assertEquals(
            BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256,
            applicationInfo.metaData.getString(SOURCE_ARCHIVE_METADATA),
        )
    }

    @Test fun packagedAppHasOnlyTheRequiredLocalDevicePermissionSurface() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val packageInfo = context.packageManager.getPackageInfo(
            context.packageName,
            PackageManager.GET_PERMISSIONS or PackageManager.GET_SERVICES,
        )
        val permissions = packageInfo.requestedPermissions?.toSet().orEmpty()
        assertFalse(Manifest.permission.INTERNET in permissions)
        assertFalse(Manifest.permission.ACCESS_COARSE_LOCATION in permissions)
        assertTrue(Manifest.permission.BLUETOOTH_SCAN in permissions)
        assertTrue(Manifest.permission.BLUETOOTH_CONNECT in permissions)
        assertTrue(Manifest.permission.POST_NOTIFICATIONS in permissions)
        assertTrue(Manifest.permission.FOREGROUND_SERVICE in permissions)
        assertTrue(Manifest.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE in permissions)
        assertTrue(Manifest.permission.ACTIVITY_RECOGNITION in permissions)
        assertTrue(Manifest.permission.VIBRATE in permissions)
        assertEquals(
            setOf("ptl.kitsu.app.notifications.KitsuConnectedDeviceService"),
            packageInfo.services.orEmpty().map { it.name }.toSet(),
        )
        assertTrue(packageInfo.services.orEmpty().all { !it.exported })
    }

    @Test fun voluntarySupportUsesTheExactExternalHttpsDestinationWithoutAnAppEntitlement() {
        val support = URI(KITSU_SUPPORT_URL)
        assertEquals("https", support.scheme)
        assertEquals("ko-fi.com", support.host)
        assertEquals("/pattalium", support.path)
        assertTrue(support.query.isNullOrEmpty())
        assertTrue(support.fragment.isNullOrEmpty())

        val intent = kitsuSupportIntent()
        assertEquals(Intent.ACTION_VIEW, intent.action)
        assertEquals(KITSU_SUPPORT_URL, intent.dataString)
        assertTrue(Intent.CATEGORY_BROWSABLE in intent.categories.orEmpty())
        assertTrue(intent.`package`.isNullOrEmpty())
        assertEquals(null, intent.component)
    }

    @Test fun savedUnlockUsesTheExactExternalHttpsDestinationWithoutInternetPermission() {
        val unlock = URI(KITSU_UNLOCK_URL)
        assertEquals("https", unlock.scheme)
        assertEquals("k32.run", unlock.host)
        assertEquals("/unlock/", unlock.path)

        val intent = kitsuUnlockIntent("K8-ABCDE-FGHJK-MNPQR")
        assertEquals(Intent.ACTION_VIEW, intent.action)
        assertEquals("https", intent.data?.scheme)
        assertEquals("k32.run", intent.data?.host)
        assertEquals("/unlock/", intent.data?.path)
        assertEquals(null, intent.data?.query)
        assertEquals("code=K8-ABCDE-FGHJK-MNPQR", intent.data?.fragment)
        assertEquals("https://k32.run/unlock/#code=K8-ABCDE-FGHJK-MNPQR", intent.dataString)
        assertTrue(Intent.CATEGORY_BROWSABLE in intent.categories.orEmpty())
        assertTrue(intent.`package`.isNullOrEmpty())
        assertEquals(null, intent.component)

        val clip = kitsuSensitiveUnlockClip("K8-ABCDE-FGHJK-MNPQR")
        assertEquals("K8-ABCDE-FGHJK-MNPQR", clip.getItemAt(0).text.toString())
        assertTrue(clip.description.extras?.getBoolean("android.content.extra.IS_SENSITIVE") == true)
    }

    @Test fun encounterCodeVaultEncryptsAndRetainsIndependentDeviceRecords() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val file = context.filesDir.resolve("encounter-code-vault-v1.bin")
        file.delete()
        try {
            val first = EncounterUnlockCode(
                deviceId = "KT12AF",
                codeId = "event:1",
                code = "K8-ABCDE-FGHJK-MNPQR",
                packId = 1,
                rarity = EncounterRarity.RARE,
                acquiredAtEpoch = 1,
            )
            val second = first.copy(
                deviceId = "KTBEEF",
                codeId = "event:2",
                code = "K8-STVWZ-23456-789AB",
            )
            AndroidKeystoreEncounterCodeVault(context).upsert(listOf(first, second))

            val raw = file.readBytes().toString(Charsets.ISO_8859_1)
            assertFalse(raw.contains(first.code))
            assertFalse(raw.contains(second.code))
            assertEquals(
                setOf("KT12AF", "KTBEEF"),
                AndroidKeystoreEncounterCodeVault(context).read().map { it.deviceId }.toSet(),
            )

            val retained = AndroidKeystoreEncounterCodeVault(context).deleteForDevice("KT12AF")
            assertEquals(listOf("KTBEEF"), retained.map { it.deviceId })
        } finally {
            file.delete()
        }
    }

    @Test fun launcherActivityIsNotLockedToPortrait() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val activityInfo = context.packageManager.getActivityInfo(
            ComponentName(context, MainActivity::class.java),
            0,
        )
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED, activityInfo.screenOrientation)
    }

    @Test fun plainTextShareTargetIsExposedWithoutAcceptingOtherMedia() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        fun resolves(type: String): Boolean = context.packageManager.queryIntentActivities(
            Intent(Intent.ACTION_SEND).setType(type),
            PackageManager.MATCH_DEFAULT_ONLY,
        ).any { it.activityInfo.packageName == context.packageName }

        assertTrue(resolves("text/plain"))
        assertFalse(resolves("image/png"))
    }

    @Test fun messageDraftStoreEncryptsAndRoundTripsTheExactDeviceBinding() = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val file = context.filesDir.resolve("message-drafts-v1.bin")
        file.delete()
        try {
            val record = MessageDraftRecord(
                deviceAddress = "AA:BB:CC:DD:EE:FF",
                threadKey = "direct:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                text = "private unsent draft",
                updatedAtMillis = 42,
            )
            val store = AndroidKeystoreMessageDraftStore(context)
            store.write(listOf(record))

            assertFalse(file.readBytes().toString(Charsets.ISO_8859_1).contains(record.text))
            assertEquals(listOf(record), store.read())
            store.write(emptyList())
            assertTrue(store.read().isEmpty())
        } finally {
            file.delete()
        }
    }

    @Test fun moderationAcceptanceAndBlocksPersistAcrossStoreRecreation() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val rawPreferences = context.getSharedPreferences(
            MODERATION_PREFERENCES,
            Context.MODE_PRIVATE,
        )
        try {
            rawPreferences.edit().clear().commit()
            val first = ModerationPreferences(context)
            assertEquals(0, first.acceptedPolicyVersion())
            assertTrue(first.blockedPeerIds().isEmpty())

            first.acceptCurrentPolicy()
            first.blockPeer("  peer-alpha  ")

            val recreated = ModerationPreferences(context)
            assertEquals(MeshUserPolicy.VERSION, recreated.acceptedPolicyVersion())
            assertEquals(setOf("peer-alpha"), recreated.blockedPeerIds())

            recreated.unblockPeer("peer-alpha")
            assertTrue(ModerationPreferences(context).blockedPeerIds().isEmpty())
        } finally {
            rawPreferences.edit().clear().commit()
        }
    }

    @Test fun disconnectSuppressionSurvivesColdServiceRecreation() = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val preferences = context.getSharedPreferences("kitsu_disconnect_${System.nanoTime()}", 0)
        try {
            ConnectionCoordinator(ProbeTransport(), AndroidReconnectSuppressionStore(preferences))
                .disconnect(suppressAutomaticReconnect = true)
            val direct = ProbeTransport()
            val cold = ConnectionCoordinator(direct, AndroidReconnectSuppressionStore(preferences))
            assertEquals("user_disconnected", cold.connect(userInitiated = false).detail)
            assertEquals(0, direct.connectCount)
            assertTrue(cold.connect(userInitiated = true).connected)
        } finally {
            preferences.edit().clear().commit()
        }
    }

    private class ProbeTransport : KitsuTransport {
        override val mode = ConnectionMode.DIRECT_BLE
        var connectCount = 0
        override suspend fun connect() = ConnectResult.Connected.also { connectCount++ }
        override suspend fun disconnect() = Unit
        override suspend fun synchronizeClock() = Unit
        override suspend fun status() = KitsuStatus(deviceId = "KTDEAD", companionName = "Kitsu", updatedAt = 1)
        override suspend fun history(after: String?, limit: Int) = HistoryPage()
        override suspend fun peers() = PeerPage()
        override suspend fun messages(after: String?, limit: Int) = MessagePage()
        override suspend fun action(command: ActionCommand) = ActionReceipt(
            command.clientRequestId,
            true,
            if (command.kind in setOf(ActionKind.SEND_MESSAGE, ActionKind.ADVERTISE_ONCE)) "queued" else "applied",
        )
        override fun events(after: String?): Flow<EventEnvelope> = emptyFlow()
        override suspend fun channels(firmwareVersion: String?): List<MeshChannel> = emptyList()
        override suspend fun configureMesh(enabled: Boolean) = MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
        override suspend fun forgetController() = ControllerForgetReceipt("kitsu.controller-forget.v1", true)
        override suspend fun firmwareUpdateStatus() = updateReceipt()
        override suspend fun beginFirmwareUpdate(manifest: ByteArray, signature: ByteArray) = updateReceipt()
        override suspend fun writeFirmwareUpdate(updateId: String, offset: Int, data: ByteArray) = updateReceipt()
        override suspend fun finishFirmwareUpdate(updateId: String) = updateReceipt()
        override suspend fun rebootFirmwareUpdate(updateId: String) = updateReceipt().copy(scheduled = true)
        override suspend fun abortFirmwareUpdate(updateId: String) = updateReceipt()
        private fun updateReceipt() = FirmwareUpdateReceipt(
            true, 1, "idle", null, "0.0.0", 0, 0, 4_096, false, false, false,
        )
    }

    private companion object {
        const val AUTHORITATIVE_APPLICATION_ID = "ptl.kitsu.app"
        const val SOURCE_ARCHIVE_METADATA = "ptl.kitsu.app.SOURCE_ARCHIVE_SHA256"
        const val MODERATION_PREFERENCES = "kitsu_mesh_moderation"
    }
}
