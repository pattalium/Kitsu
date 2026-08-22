package app.kitsu.mobile.connection

import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.MockKitsuTransport
import app.kitsu.mobile.model.LanState
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
class ConnectionCoordinatorTest {
    @Test fun absentBondedCompanionAllowsBackendFallback() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE, ConnectResult.CompanionAbsent)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val state = ConnectionCoordinator(direct, backend).connect()
        assertTrue(state.connected)
        assertEquals(ConnectionMode.REMOTE_BACKEND, state.mode)
        assertEquals(1, backend.connectCount)
    }

    @Test fun directFailureNeverFallsBack() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE, ConnectResult.Failed("mac_rejected"))
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val state = ConnectionCoordinator(direct, backend).connect()
        assertFalse(state.connected)
        assertEquals("mac_rejected", state.detail)
        assertEquals(0, backend.connectCount)
    }

    @Test fun permissionFailureNeverFallsBack() = runTest {
        val direct = MockKitsuTransport(
            ConnectionMode.DIRECT_BLE,
            ConnectResult.PermissionRequired(listOf("bluetooth")),
        )
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val state = ConnectionCoordinator(direct, backend).connect()
        assertEquals(ConnectionMode.PERMISSION_REQUIRED, state.mode)
        assertEquals(0, backend.connectCount)
    }

    @Test fun missingOsBondRequiresRepairAndNeverFallsBack() = runTest {
        val direct = MockKitsuTransport(
            ConnectionMode.DIRECT_BLE,
            ConnectResult.Failed("bond_missing_repair_required"),
        )
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val state = ConnectionCoordinator(direct, backend).connect()
        assertFalse(state.connected)
        assertEquals("bond_missing_repair_required", state.detail)
        assertEquals(0, backend.connectCount)
    }

    @Test fun userDisconnectClosesEveryPathAndSuppressesAutomaticReconnect() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        assertTrue(coordinator.connect().connected)

        coordinator.disconnect(suppressAutomaticReconnect = true)
        val directConnects = direct.connectCount
        val backendConnects = backend.connectCount

        val suppressed = coordinator.connect(userInitiated = false)
        assertFalse(suppressed.connected)
        assertEquals("user_disconnected", suppressed.detail)
        assertTrue(coordinator.isAutomaticReconnectSuppressed())
        assertEquals(directConnects, direct.connectCount)
        assertEquals(backendConnects, backend.connectCount)
        assertTrue(direct.disconnectCount >= 2)
        assertTrue(backend.disconnectCount >= 2)
    }

    @Test fun explicitConnectClearsUserDisconnectSuppression() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.disconnect(suppressAutomaticReconnect = true)

        val connected = coordinator.connect(userInitiated = true)

        assertTrue(connected.connected)
        assertFalse(coordinator.isAutomaticReconnectSuppressed())
        assertEquals(1, direct.connectCount)
        assertEquals(0, backend.connectCount)
    }

    @Test fun explicitRemoteChoiceBypassesNearbyBleAndClearsSuppression() = runTest {
        val store = InMemoryReconnectSuppressionStore()
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend, store)
        coordinator.disconnect(suppressAutomaticReconnect = true)

        val connected = coordinator.connectRemote(userInitiated = true)

        assertTrue(connected.connected)
        assertEquals(ConnectionMode.REMOTE_BACKEND, connected.mode)
        assertEquals("owner_selected_remote_service", connected.detail)
        assertEquals(0, direct.connectCount)
        assertEquals(1, backend.connectCount)
        assertFalse(coordinator.isAutomaticReconnectSuppressed())
    }

    @Test fun explicitRemoteChoiceReportsBackendFailureWithoutTryingBle() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(
            ConnectionMode.REMOTE_BACKEND,
            ConnectResult.Failed("sign_in_required"),
        )

        val result = ConnectionCoordinator(direct, backend).connectRemote()

        assertFalse(result.connected)
        assertEquals("sign_in_required", result.detail)
        assertTrue(result.explicitRemoteAttempt)
        assertEquals(0, direct.connectCount)
        assertEquals(1, backend.connectCount)
    }

    @Test fun switchingToRemoteClosesTheActiveBleSessionFirst() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        assertEquals(ConnectionMode.DIRECT_BLE, coordinator.connect().mode)
        val disconnectsBeforeSwitch = direct.disconnectCount

        val remote = coordinator.connectRemote()

        assertEquals(ConnectionMode.REMOTE_BACKEND, remote.mode)
        assertTrue(direct.disconnectCount > disconnectsBeforeSwitch)
        assertEquals(1, direct.connectCount)
        assertEquals(1, backend.connectCount)
    }

    @Test fun suppressionSurvivesCoordinatorRecreationAndOnlyExplicitConnectClearsIt() = runTest {
        val store = InMemoryReconnectSuppressionStore()
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val firstProcess = ConnectionCoordinator(direct, backend, store)

        firstProcess.disconnect(suppressAutomaticReconnect = true)

        val coldRelaunch = ConnectionCoordinator(direct, backend, store)
        assertTrue(coldRelaunch.isAutomaticReconnectSuppressed())
        assertEquals("user_disconnected", coldRelaunch.connect(userInitiated = false).detail)
        assertEquals(0, direct.connectCount)

        // Authentication/lifecycle teardown is not an explicit Connect and must preserve it.
        coldRelaunch.disconnect(suppressAutomaticReconnect = false)
        val anotherRelaunch = ConnectionCoordinator(direct, backend, store)
        assertEquals("user_disconnected", anotherRelaunch.connect(userInitiated = false).detail)
        assertEquals(0, direct.connectCount)

        assertTrue(anotherRelaunch.connect(userInitiated = true).connected)
        val afterExplicitConnect = ConnectionCoordinator(direct, backend, store)
        assertTrue(afterExplicitConnect.connect(userInitiated = false).connected)
        assertFalse(afterExplicitConnect.isAutomaticReconnectSuppressed())
    }

    @Test fun newPreferenceStoreDefaultsToAutomaticConnectionAllowed() = runTest {
        val store = InMemoryReconnectSuppressionStore()
        val coordinator = ConnectionCoordinator(
            MockKitsuTransport(ConnectionMode.DIRECT_BLE),
            MockKitsuTransport(ConnectionMode.REMOTE_BACKEND),
            store,
        )

        assertFalse(coordinator.isAutomaticReconnectSuppressed())
        assertTrue(coordinator.connect(userInitiated = false).connected)
    }

    @Test fun unauthenticatedTeardownDoesNotClearUserDisconnectAcrossSignIn() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.disconnect(suppressAutomaticReconnect = true)

        // Repository.handleSignedOut uses this teardown form; it must retain
        // the explicit Disconnect choice when authentication later changes.
        coordinator.disconnect(suppressAutomaticReconnect = false)
        val afterSignIn = coordinator.connect(userInitiated = false)

        assertFalse(afterSignIn.connected)
        assertEquals("user_disconnected", afterSignIn.detail)
        assertTrue(coordinator.isAutomaticReconnectSuppressed())
        assertEquals(0, direct.connectCount)
        assertEquals(0, backend.connectCount)
    }

    @Test fun cancellingAnInFlightConnectTearsDownBothPaths() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE).apply {
            connectDelayMillis = 60_000
        }
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        val connecting = launch { coordinator.connect() }
        runCurrent()

        connecting.cancelAndJoin()

        assertFalse(coordinator.state.value.connected)
        assertEquals("connection_cancelled", coordinator.state.value.detail)
        assertTrue(direct.disconnectCount >= 2)
        assertTrue(backend.disconnectCount >= 2)
        assertEquals(0, backend.connectCount)
    }

    @Test fun enrollmentBackendPollingRequiresAConfirmedBleAbsence() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE, ConnectResult.CompanionAbsent)
        val backend = MockKitsuTransport(
            ConnectionMode.REMOTE_BACKEND,
            ConnectResult.Failed("no_remote_companion"),
        )
        val coordinator = ConnectionCoordinator(direct, backend)

        assertEquals("no_remote_companion", coordinator.connect().detail)
        backend.connectResult = ConnectResult.Connected
        val polled = coordinator.pollBackendAfterConfirmedAbsence()

        assertTrue(polled.connected)
        assertEquals(ConnectionMode.REMOTE_BACKEND, polled.mode)
        assertEquals(1, direct.connectCount)
        assertEquals(2, backend.connectCount)
    }

    @Test fun userDisconnectRevokesEnrollmentBackendPollingAuthorization() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE, ConnectResult.CompanionAbsent)
        val backend = MockKitsuTransport(
            ConnectionMode.REMOTE_BACKEND,
            ConnectResult.Failed("no_remote_companion"),
        )
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.connect()
        coordinator.disconnect(suppressAutomaticReconnect = true)

        backend.connectResult = ConnectResult.Connected
        val polled = coordinator.pollBackendAfterConfirmedAbsence()

        assertFalse(polled.connected)
        assertEquals("backend_poll_not_authorized", polled.detail)
        assertEquals(1, backend.connectCount)
    }

    @Test fun explicitEnrollmentHandoffCompletesOnlyOnBoundAuthenticatedRemotePath() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND).apply {
            mockStatus = mockStatus.copy(
                deviceId = "KTDEAD",
                lan = LanState(
                    online = true,
                    provenance = "gateway_mtls_device_hmac",
                    gatewayId = "00112233-4455-6677-8899-aabbccddeeff",
                    lastSeenAt = "2026-08-18T10:00:00Z",
                ),
            )
        }
        val coordinator = ConnectionCoordinator(direct, backend)
        assertTrue(coordinator.connect().connected)

        coordinator.beginEnrollmentRemoteHandoff()
        val result = coordinator.pollEnrollmentBackend(
            expectedDeviceId = "KTDEAD",
            expectedGatewayId = "00112233-4455-6677-8899-aabbccddeeff",
        )

        assertTrue(result.connected)
        assertEquals(ConnectionMode.REMOTE_BACKEND, result.mode)
        assertEquals("enrollment_authenticated_remote_path", result.detail)
    }

    @Test fun enrollmentHandoffRejectsGenericRemoteConnectedResult() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND).apply {
            mockStatus = mockStatus.copy(
                deviceId = "KTDEAD",
                lan = LanState(
                    online = true,
                    provenance = "gateway_mtls_device_hmac",
                    gatewayId = "11112233-4455-6677-8899-aabbccddeeff",
                    lastSeenAt = "2026-08-18T10:00:00Z",
                ),
            )
        }
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.connect()
        coordinator.beginEnrollmentRemoteHandoff()

        val result = coordinator.pollEnrollmentBackend(
            expectedDeviceId = "KTDEAD",
            expectedGatewayId = "00112233-4455-6677-8899-aabbccddeeff",
        )

        assertFalse(result.connected)
        assertEquals("remote_gateway_binding_failed", result.detail)
        assertTrue(backend.disconnectCount >= 2)
    }

    @Test fun userDisconnectRevokesExplicitEnrollmentHandoff() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE)
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.connect()
        coordinator.beginEnrollmentRemoteHandoff()
        coordinator.disconnect(suppressAutomaticReconnect = true)

        val result = coordinator.pollEnrollmentBackend(
            expectedDeviceId = "KTDEAD",
            expectedGatewayId = "00112233-4455-6677-8899-aabbccddeeff",
        )

        assertFalse(result.connected)
        assertEquals("backend_poll_not_authorized", result.detail)
        assertEquals(0, backend.connectCount)
    }

    @Test fun publicGatewayHandoffIsAddressScopedAndProtectedUntilRelease() = runTest {
        val direct = MockKitsuTransport(ConnectionMode.DIRECT_BLE).apply {
            connectedAddress = "00:11:22:33:44:55"
        }
        val backend = MockKitsuTransport(ConnectionMode.REMOTE_BACKEND)
        val coordinator = ConnectionCoordinator(direct, backend)
        coordinator.connect()
        val beforeHandoff = direct.disconnectCount

        assertFalse(coordinator.handoffDirectForPublicGateway("AA:BB:CC:DD:EE:FF"))
        assertEquals(beforeHandoff + 1, direct.disconnectCount)

        coordinator.connect(userInitiated = true)
        val beforeMatchingHandoff = direct.disconnectCount
        assertTrue(coordinator.handoffDirectForPublicGateway("00:11:22:33:44:55"))
        coordinator.disconnect()
        assertEquals(beforeMatchingHandoff, direct.disconnectCount)
        coordinator.completePublicGatewayHandoff()
        assertEquals(beforeMatchingHandoff + 1, direct.disconnectCount)
    }
}
