package app.kitsu.mobile

import android.graphics.drawable.AdaptiveIconDrawable
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import app.kitsu.mobile.connection.AndroidReconnectSuppressionStore
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.MeshPeerKeyPolicy
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.KitsuTransport
import app.kitsu.mobile.transport.RemoteSnapshotPolicy
import app.kitsu.mobile.ui.MessageComposerPolicy
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Host-independent release contract probes run on every supported emulator API. */
@RunWith(AndroidJUnit4::class)
class ReleaseContractInstrumentationTest {
    @Test fun launcherLoadsAsAnAdaptiveIconOnEverySupportedApi() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val icon = context.packageManager.getApplicationIcon(context.applicationInfo)
        assertTrue(icon is AdaptiveIconDrawable)
        icon as AdaptiveIconDrawable
        assertNotNull(icon.background)
        assertNotNull(icon.foreground)
        if (Build.VERSION.SDK_INT >= 33) {
            assertNotNull(icon.monochrome)
        }
    }

    @Test fun productionBuildCarriesTheExactRealmIssuer() {
        assertEquals(7, BuildConfig.VERSION_CODE)
        assertTrue(BuildConfig.VERSION_NAME.startsWith("1.1.1"))
        assertEquals("https://auth.k32.run/realms/kitsu", BuildConfig.KITSU_OIDC_ISSUER)
        assertTrue(
            BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256 == "unbound" ||
                Regex("^[0-9a-f]{64}$").matches(BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256),
        )
    }

    @Test fun disconnectSuppressionSurvivesAuthTeardownUntilExplicitReconnect() = runBlocking {
        val direct = ProbeTransport(ConnectionMode.DIRECT_BLE)
        val backend = ProbeTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)

        coordinator.disconnect(suppressAutomaticReconnect = true)
        coordinator.disconnect(suppressAutomaticReconnect = false) // sign-out teardown
        val automatic = coordinator.connect(userInitiated = false) // sign-in lifecycle callback

        assertFalse(automatic.connected)
        assertEquals("user_disconnected", automatic.detail)
        assertEquals(0, direct.connectCount)
        assertEquals(0, backend.connectCount)

        assertTrue(coordinator.connect(userInitiated = true).connected)
        assertFalse(coordinator.isAutomaticReconnectSuppressed())
    }

    @Test fun disconnectSuppressionSurvivesColdServiceRecreationOnAndroidStorage() = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val preferences = context.getSharedPreferences(
            "kitsu_disconnect_contract_${System.nanoTime()}",
            android.content.Context.MODE_PRIVATE,
        )
        try {
            val firstProcess = ConnectionCoordinator(
                ProbeTransport(ConnectionMode.DIRECT_BLE),
                ProbeTransport(ConnectionMode.REMOTE_BACKEND),
                AndroidReconnectSuppressionStore(preferences),
            )
            firstProcess.disconnect(suppressAutomaticReconnect = true)

            val coldRelaunchDirect = ProbeTransport(ConnectionMode.DIRECT_BLE)
            val coldRelaunch = ConnectionCoordinator(
                coldRelaunchDirect,
                ProbeTransport(ConnectionMode.REMOTE_BACKEND),
                AndroidReconnectSuppressionStore(preferences),
            )
            assertEquals("user_disconnected", coldRelaunch.connect(userInitiated = false).detail)
            assertEquals(0, coldRelaunchDirect.connectCount)

            // Only this explicit Connect writes false to the durable preference.
            assertTrue(coldRelaunch.connect(userInitiated = true).connected)
            val nextRelaunch = ConnectionCoordinator(
                ProbeTransport(ConnectionMode.DIRECT_BLE),
                ProbeTransport(ConnectionMode.REMOTE_BACKEND),
                AndroidReconnectSuppressionStore(preferences),
            )
            assertTrue(nextRelaunch.connect(userInitiated = false).connected)
        } finally {
            assertTrue(preferences.edit().clear().commit())
        }
    }

    @Test fun reconnectPreferenceMigrationHasExplicitDefaultsAndFailsClosedOnNewerSchema() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val preferences = context.getSharedPreferences(
            "kitsu_disconnect_migration_${System.nanoTime()}",
            android.content.Context.MODE_PRIVATE,
        )
        try {
            val fresh = AndroidReconnectSuppressionStore(preferences)
            assertFalse(fresh.readSuppressed())
            assertEquals(
                AndroidReconnectSuppressionStore.CURRENT_SCHEMA_VERSION,
                preferences.getInt(AndroidReconnectSuppressionStore.KEY_SCHEMA_VERSION, 0),
            )

            assertTrue(preferences.edit().clear()
                .putBoolean(AndroidReconnectSuppressionStore.KEY_SUPPRESSED, true)
                .commit())
            assertTrue(AndroidReconnectSuppressionStore(preferences).readSuppressed())
            assertEquals(
                AndroidReconnectSuppressionStore.CURRENT_SCHEMA_VERSION,
                preferences.getInt(AndroidReconnectSuppressionStore.KEY_SCHEMA_VERSION, 0),
            )

            assertTrue(preferences.edit().clear()
                .putInt(AndroidReconnectSuppressionStore.KEY_SCHEMA_VERSION, 999)
                .putBoolean(AndroidReconnectSuppressionStore.KEY_SUPPRESSED, false)
                .commit())
            assertTrue(AndroidReconnectSuppressionStore(preferences).readSuppressed())
        } finally {
            assertTrue(preferences.edit().clear().commit())
        }
    }

    @Test fun peerAndChannelComposerFormatsAreCanonical() {
        val canonical = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        assertTrue(MeshPeerKeyPolicy.isCanonicalBase64Url(canonical))
        assertFalse(
            MeshPeerKeyPolicy.isCanonicalBase64Url(
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB",
            ),
        )
        assertEquals(canonical, MeshPeerKeyPolicy.migrateLegacyHex("00".repeat(32)))
        assertNull(
            MessageComposerPolicy.validationError(MessageRoute.DIRECT, canonical, "hello"),
        )
        assertTrue(
            MessageComposerPolicy.contactRecipients(listOf(Peer(canonical, "Alice")))
                .single().reference == canonical,
        )
        assertEquals(
            listOf("0"),
            MessageComposerPolicy.channelRecipients(
                listOf(
                    MeshChannel(0, true, "Public"),
                    MeshChannel(1, null, null),
                    MeshChannel(2, false, null),
                    MeshChannel(3, null, null),
                ),
            ).map { it.reference },
        )
    }

    @Test fun authenticatedRemoteSnapshotMustCarryExactDeviceGatewayAndFreshnessFields() {
        val status = ProbeTransport.authenticatedStatus()
        assertNull(
            RemoteSnapshotPolicy.validationError(
                status,
                expectedDeviceId = "KTDEAD",
                expectedGatewayId = "00112233-4455-6677-8899-aabbccddeeff",
            ),
        )
        assertEquals(
            "remote_provenance_unverified",
            RemoteSnapshotPolicy.validationError(
                status.copy(lan = status.lan.copy(provenance = "gateway_claim_token")),
            ),
        )
        assertEquals(
            "remote_companion_offline",
            RemoteSnapshotPolicy.validationError(status.copy(lan = status.lan.copy(online = null))),
        )
    }

    private class ProbeTransport(
        override val mode: ConnectionMode,
        var result: ConnectResult = ConnectResult.Connected,
        var currentStatus: KitsuStatus = authenticatedStatus(),
    ) : KitsuTransport {
        var connectCount = 0
        override suspend fun connect(): ConnectResult = result.also { connectCount += 1 }
        override suspend fun disconnect() = Unit
        override suspend fun status(): KitsuStatus = currentStatus
        override suspend fun history(after: String?, limit: Int) = HistoryPage()
        override suspend fun peers() = PeerPage()
        override suspend fun messages(after: String?, limit: Int) = MessagePage()
        override suspend fun action(command: ActionCommand) =
            ActionReceipt(command.clientRequestId, true, "accepted")
        override fun events(after: String?): Flow<EventEnvelope> = emptyFlow()
        override suspend fun channels(): List<MeshChannel> = emptyList()
        override suspend fun configureMesh(enabled: Boolean) =
            MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
        override suspend fun provisionWifi(credentials: WifiProvisioning) =
            ProvisioningReceipt(true, "stored")
        override suspend fun configureGateway(configuration: GatewayConfiguration) =
            GatewayConfigurationReceipt(true, "stored")
        override suspend fun beginGatewayEnrollment(request: GatewayEnrollmentBeginBody) =
            GatewayEnrollmentReceipt(
                "kitsu.gateway-enrollment.receipt.v1",
                false,
                "physical_confirmation_required",
                request.enrollmentId,
                30_000,
                "physical_confirmation_required",
            )
        override suspend fun finishGatewayEnrollment(request: GatewayEnrollmentFinishBody) =
            GatewayEnrollmentReceipt(
                "kitsu.gateway-enrollment.receipt.v1",
                true,
                "ready_for_wifi",
                request.enrollmentId,
                300_000,
            )

        companion object {
            fun authenticatedStatus() = KitsuStatus(
                deviceId = "KTDEAD",
                displayName = "Kitsu",
                lan = LanState(
                    online = true,
                    provenance = "gateway_mtls_device_hmac",
                    gatewayId = "00112233-4455-6677-8899-aabbccddeeff",
                    lastSeenAt = "2026-08-18T10:00:00Z",
                ),
                updatedAt = 1_787_047_200,
            )
        }
    }
}
