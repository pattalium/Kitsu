package app.kitsu.mobile.ui

import app.kitsu.mobile.connection.ConnectionState
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.transport.ConnectionMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OwnerAccountUiPolicyTest {
    @Test fun signedOutOffersOneClearRemoteAccessAction() {
        val presentation = OwnerAccountUiPolicy.presentation(OwnerAccountStatus.SIGNED_OUT)
        assertTrue(presentation.signInEnabled)
        assertFalse(presentation.signOutEnabled)
        assertTrue(presentation.statusDetail.contains("Bluetooth remains available"))
    }

    @Test fun signedInOffersSignOutAndExplainsItsScope() {
        val presentation = OwnerAccountUiPolicy.presentation(OwnerAccountStatus.SIGNED_IN)
        assertFalse(presentation.signInEnabled)
        assertTrue(presentation.signOutEnabled)
        assertTrue(OwnerAccountUiPolicy.PURPOSE.contains("Wi-Fi gateway"))
    }

    @Test fun ownerCopyAnswersAcquisitionAndRecoveryWithoutProtocolJargon() {
        val copy = listOf(
            OwnerAccountUiPolicy.PURPOSE,
            OwnerAccountUiPolicy.BLUETOOTH_BOUNDARY,
            OwnerAccountUiPolicy.INITIAL_ACCESS,
            OwnerAccountUiPolicy.RECOVERY,
        ).joinToString(" ")
        assertTrue(copy.contains("one-time owner username and temporary password"))
        assertTrue(copy.contains("first sign-in"))
        assertTrue(copy.contains("no public sign-up", ignoreCase = true))
        assertFalse(Regex("OIDC|PKCE|Keystore|token", RegexOption.IGNORE_CASE).containsMatchIn(copy))
    }

    @Test fun missingRemoteSessionNeverClaimsBluetoothNeedsAnAccount() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.OFFLINE,
                    connected = false,
                    detail = "sign_in_required",
                ),
            ),
        )
        assertEquals("No nearby Kitsu", presentation.title)
        assertTrue(presentation.detail.contains("Connect over Bluetooth"))
        assertTrue(presentation.detail.contains("sign in under More"))
    }

    @Test fun explicitRemoteChoiceIsNotMisreportedAsBleAbsenceFallback() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.REMOTE_BACKEND,
                    connected = true,
                    detail = "owner_selected_remote_service",
                ),
            ),
        )

        assertEquals("Connected remotely", presentation.title)
        assertTrue(presentation.detail.contains("you selected"))
        assertFalse(presentation.detail.contains("absent"))
    }

    @Test fun explicitRemoteConnectionDoesNotClaimItIsScanningBluetooth() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.CONNECTING,
                    connected = false,
                    detail = "checking_authenticated_remote_service",
                ),
            ),
        )

        assertEquals("Connecting via Wi-Fi", presentation.title)
        assertTrue(presentation.detail.contains("owner service"))
        assertFalse(presentation.detail.contains("Bluetooth"))
    }

    @Test fun explicitRemoteFailureDoesNotInventAnAbsentBleScan() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.OFFLINE,
                    connected = false,
                    detail = "sign_in_required",
                    explicitRemoteAttempt = true,
                ),
            ),
        )

        assertEquals("Remote connection failed", presentation.title)
        assertEquals("Sign in to use the Wi-Fi gateway connection.", presentation.detail)
        assertFalse(presentation.detail.contains("Bluetooth"))
        assertFalse(presentation.detail.contains("absent"))
    }
}
