package app.kitsu.mobile.transport

import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class RemoteSnapshotPolicyTest {
    private val status = KitsuStatus(
        deviceId = "KTDEAD",
        displayName = "Kitsu",
        lan = LanState(
            online = true,
            provenance = RemoteSnapshotPolicy.AUTHENTICATED_PROVENANCE,
            gatewayId = "00112233-4455-6677-8899-aabbccddeeff",
            lastSeenAt = "2026-08-18T10:00:00Z",
        ),
        updatedAt = 1_787_050_800,
    )

    @Test fun acceptsOnlyExactAuthenticatedDeviceAndGatewayBinding() {
        assertNull(
            RemoteSnapshotPolicy.validationError(
                status,
                expectedDeviceId = "KTDEAD",
                expectedGatewayId = "00112233-4455-6677-8899-aabbccddeeff",
            ),
        )
        assertEquals(
            "remote_companion_binding_failed",
            RemoteSnapshotPolicy.validationError(status, expectedDeviceId = "OTHER"),
        )
        assertEquals(
            "remote_gateway_binding_failed",
            RemoteSnapshotPolicy.validationError(
                status,
                expectedGatewayId = "11112233-4455-6677-8899-aabbccddeeff",
            ),
        )
    }

    @Test fun rejectsOfflineUnprovenMissingGatewayAndMalformedTime() {
        assertEquals(
            "remote_companion_offline",
            RemoteSnapshotPolicy.validationError(status.copy(lan = status.lan.copy(online = false))),
        )
        assertEquals(
            "remote_provenance_unverified",
            RemoteSnapshotPolicy.validationError(status.copy(lan = status.lan.copy(provenance = "gateway_only"))),
        )
        assertEquals(
            "remote_gateway_unverified",
            RemoteSnapshotPolicy.validationError(status.copy(lan = status.lan.copy(gatewayId = null))),
        )
        assertEquals(
            "remote_last_seen_invalid",
            RemoteSnapshotPolicy.validationError(status.copy(lan = status.lan.copy(lastSeenAt = "yesterday"))),
        )
    }
}
