package app.kitsu.mobile.ui

import app.kitsu.mobile.connection.ConnectionState
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.transport.ConnectionMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WifiStatusPresentationTest {
    @Test fun storedCredentialsAreNotMisreportedAsARejectedWrite() {
        val presentation = wifiStatusPresentation(
            ownerState(
                LanState(
                    wifiConfigured = true,
                    wifiState = "unconfigured",
                    gatewayConfigured = false,
                    remoteConnectivityAllowed = true,
                ),
            ),
        )

        assertEquals("stored and verified", presentation.credentials)
        assertEquals("credentials stored; association has not started yet", presentation.link)
        assertTrue(presentation.nextStep.orEmpty().contains("join Wi-Fi now"))
        assertTrue(presentation.nextStep.orEmpty().contains("store gateway trust"))
    }

    @Test fun legacyBlePriorityStateIsExplainedInsteadOfLookingLikeLostStorage() {
        val state = ownerState(
            LanState(
                wifiConfigured = true,
                wifiState = "ble_active",
                gatewayConfigured = true,
                gatewayEnrolled = true,
                remoteConnectivityAllowed = true,
            ),
            ConnectionMode.DIRECT_BLE,
        )
        val presentation = wifiStatusPresentation(state)

        assertTrue(presentation.link.contains("installed firmware"))
        assertTrue(presentation.nextStep.orEmpty().contains("Update Kitsu firmware"))
        assertTrue(!wifiRemoteHandoffReady(state))
    }

    @Test fun backoffDoesNotClaimRemoteHandoffIsReady() {
        val state = ownerState(
            LanState(
                wifiConfigured = true,
                wifiState = "backoff",
                gatewayConfigured = true,
                gatewayEnrolled = true,
                remoteConnectivityAllowed = true,
            ),
            ConnectionMode.DIRECT_BLE,
        )

        assertTrue(!wifiRemoteHandoffReady(state))
        assertTrue(wifiStatusPresentation(state).nextStep.orEmpty().contains("not connected yet"))
    }

    @Test fun remotePathNamesBothAndroidOwnerServiceAndDeviceWifi() {
        val presentation = wifiStatusPresentation(
            ownerState(
                LanState(
                    wifiConfigured = true,
                    wifiState = "connected",
                    gatewayConfigured = true,
                    gatewayEnrolled = true,
                    remoteConnectivityAllowed = true,
                ),
                ConnectionMode.REMOTE_BACKEND,
            ),
        )

        assertEquals("connected", presentation.link)
        assertTrue(presentation.nextStep.orEmpty().contains("owner service"))
        assertTrue(presentation.nextStep.orEmpty().contains("Wi-Fi gateway"))
    }

    private fun ownerState(
        lan: LanState,
        mode: ConnectionMode = ConnectionMode.DIRECT_BLE,
    ) = OwnerState(
        connection = ConnectionState(mode = mode, connected = true),
        status = KitsuStatus(
            deviceId = "KTTEST",
            displayName = "Kitsu",
            lan = lan,
            updatedAt = 1,
        ),
    )
}
