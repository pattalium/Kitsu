package ptl.kitsu.app.connection

import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.MockKitsuTransport
import ptl.kitsu.app.transport.TransportException
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.AdvertiseScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class ConnectionCoordinatorTest {
    @Test fun connectsOnlyTheDirectTransport() = runTest {
        val direct = MockKitsuTransport()
        val state = ConnectionCoordinator(direct).connect(userInitiated = true)
        assertTrue(state.connected)
        assertEquals(ConnectionMode.DIRECT_BLE, state.mode)
        assertEquals(1, direct.connectCount)
    }

    @Test fun absentSelectedKitsuStaysOffline() = runTest {
        val direct = MockKitsuTransport(connectResult = ConnectResult.CompanionAbsent)
        val state = ConnectionCoordinator(direct).connect(userInitiated = true)
        assertFalse(state.connected)
        assertEquals("selected_kitsu_absent", state.detail)
    }

    @Test fun bluetoothPermissionIsTruthful() = runTest {
        val direct = MockKitsuTransport(
            connectResult = ConnectResult.PermissionRequired(listOf("android.permission.BLUETOOTH_CONNECT")),
        )
        val state = ConnectionCoordinator(direct).connect(userInitiated = true)
        assertEquals(ConnectionMode.PERMISSION_REQUIRED, state.mode)
        assertFalse(state.connected)
    }

    @Test fun boundGattLossBeforeConnectedReturnCannotResurrectTheSession() = runTest {
        val direct = ClockAwareTransport()
        val coordinator = ConnectionCoordinator(direct)
        direct.beforeConnectReturn = {
            coordinator.onDirectTransportDisconnected("gatt_disconnected")
        }

        val state = coordinator.connect(userInitiated = true)

        assertFalse(state.connected)
        assertEquals(ConnectionMode.OFFLINE, state.mode)
        assertEquals("gatt_disconnected", state.detail)
        assertEquals(state, coordinator.state.value)
    }

    @Test fun boundGattLossDetailOutranksGenericFailedConnectReturn() = runTest {
        val direct = ClockAwareTransport().apply {
            connectResultAfterCallback = ConnectResult.Failed("direct_connect_failed")
        }
        val coordinator = ConnectionCoordinator(direct)
        direct.beforeConnectReturn = {
            coordinator.onDirectTransportDisconnected("gatt_status_133")
        }

        val state = coordinator.connect(userInitiated = true)

        assertFalse(state.connected)
        assertEquals("gatt_status_133", state.detail)
        assertEquals(state, coordinator.state.value)
    }

    @Test fun durableDisconnectBlocksColdAutomaticAttemptUntilExplicitConnect() = runTest {
        val store = InMemoryReconnectSuppressionStore()
        val direct = MockKitsuTransport()
        val first = ConnectionCoordinator(direct, store)
        first.disconnect(suppressAutomaticReconnect = true)

        val coldDirect = MockKitsuTransport()
        val cold = ConnectionCoordinator(coldDirect, store)
        assertEquals("user_disconnected", cold.connect(userInitiated = false).detail)
        assertEquals(0, coldDirect.connectCount)
        assertTrue(cold.connect(userInitiated = true).connected)
        assertFalse(cold.isAutomaticReconnectSuppressed())
    }

    @Test fun disconnectClosesTheSingleGattOwner() = runTest {
        val direct = MockKitsuTransport()
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)
        coordinator.disconnect(suppressAutomaticReconnect = true)
        assertFalse(coordinator.state.value.connected)
        assertTrue(direct.disconnectCount >= 2)
    }

    @Test fun unsolicitedGattLossPublishesOfflineState() = runTest {
        val direct = ClockAwareTransport(initialWarning = "system_clock_failed")
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)
        assertEquals("system_clock_failed", coordinator.state.value.warning)
        coordinator.onDirectTransportDisconnected("gatt_disconnected")
        assertFalse(coordinator.state.value.connected)
        assertEquals("gatt_disconnected", coordinator.state.value.detail)
        assertEquals(null, coordinator.state.value.warning)
    }

    @Test fun malformedAuthenticatedFramePublishesOfflineState() = runTest {
        val coordinator = ConnectionCoordinator(MockKitsuTransport())
        coordinator.connect(userInitiated = true)
        coordinator.onDirectTransportDisconnected("malformed_response")
        assertFalse(coordinator.state.value.connected)
        assertEquals("malformed_response", coordinator.state.value.detail)
    }

    @Test fun authenticatedClockRecoveryUsesTheActiveDirectTransport() = runTest {
        val direct = MockKitsuTransport()
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)

        coordinator.synchronizeClock()

        assertEquals(1, direct.clockSyncCount)
        assertTrue(coordinator.state.value.connected)
    }

    @Test fun connectedClockWarningIsVisibleAndSuccessfulRetryClearsIt() = runTest {
        val direct = ClockAwareTransport(initialWarning = "system_clock_failed")
        val coordinator = ConnectionCoordinator(direct)

        val connected = coordinator.connect(userInitiated = true)
        assertTrue(connected.connected)
        assertEquals("system_clock_failed", connected.warning)

        coordinator.synchronizeClock()

        assertTrue(coordinator.state.value.connected)
        assertEquals(null, coordinator.state.value.warning)
        assertEquals(1, direct.clockSyncCount)
    }

    @Test fun retryableClockFailureKeepsTheSessionAndExactCause() = runTest {
        val direct = ClockAwareTransport()
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)
        val disconnectsBeforeFailure = direct.disconnectCount
        direct.nextClockFailure = "sequence_violation"

        val failure = try {
            coordinator.synchronizeClock()
            null
        } catch (error: TransportException) {
            error
        }

        assertEquals("sequence_violation", failure?.code)
        assertTrue(coordinator.state.value.connected)
        assertEquals("sequence_violation", coordinator.state.value.warning)
        assertEquals(disconnectsBeforeFailure, direct.disconnectCount)
    }

    @Test fun meshActionClockFailureHasNoSideEffectAndNextRetryUsesSameSession() = runTest {
        val direct = ClockAwareTransport()
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)
        val disconnectsBeforeFailure = direct.disconnectCount
        direct.nextClockFailure = "system_clock_failed"

        val first = ActionCommand(
            kind = ActionKind.ADVERTISE_ONCE,
            clientRequestId = "00000000-0000-4000-8000-000000000001",
            advertiseScope = AdvertiseScope.MESH,
        )
        val failure = try {
            coordinator.withTransport { it.action(first) }
            null
        } catch (error: TransportException) {
            error
        }
        assertEquals("system_clock_failed", failure?.code)
        assertTrue(coordinator.state.value.connected)
        assertEquals("system_clock_failed", coordinator.state.value.warning)
        assertEquals(0, direct.appliedActions)
        assertEquals(disconnectsBeforeFailure, direct.disconnectCount)

        val second = first.copy(clientRequestId = "00000000-0000-4000-8000-000000000002")
        val receipt = coordinator.withTransport { it.action(second) }

        assertTrue(receipt.accepted)
        assertEquals(1, direct.appliedActions)
        assertTrue(coordinator.state.value.connected)
        assertEquals(null, coordinator.state.value.warning)
        assertEquals(disconnectsBeforeFailure, direct.disconnectCount)
    }

    @Test fun concurrentGattLossWinsOverClockWarningPublication() = runTest {
        val direct = ClockAwareTransport()
        val coordinator = ConnectionCoordinator(direct)
        coordinator.connect(userInitiated = true)
        direct.nextClockFailure = "system_clock_failed"
        direct.onConnectionWarningRead = {
            coordinator.onDirectTransportDisconnected("gatt_disconnected")
        }

        try {
            coordinator.synchronizeClock()
            fail("expected system_clock_failed")
        } catch (error: TransportException) {
            assertEquals("system_clock_failed", error.code)
        }

        assertFalse(coordinator.state.value.connected)
        assertEquals(ConnectionMode.OFFLINE, coordinator.state.value.mode)
        assertEquals("gatt_disconnected", coordinator.state.value.detail)
        assertEquals(null, coordinator.state.value.warning)
    }

    private class ClockAwareTransport(
        private val initialWarning: String? = null,
        private val delegate: MockKitsuTransport = MockKitsuTransport(),
    ) : KitsuTransport by delegate {
        private var connected = false
        private var warning: String? = null
        var nextClockFailure: String? = null
        var onConnectionWarningRead: (() -> Unit)? = null
        var beforeConnectReturn: (() -> Unit)? = null
        var connectResultAfterCallback: ConnectResult = ConnectResult.Connected
        var clockSyncCount = 0
        var disconnectCount = 0
        var appliedActions = 0

        override suspend fun connect(): ConnectResult {
            connected = connectResultAfterCallback == ConnectResult.Connected
            warning = initialWarning
            beforeConnectReturn?.invoke()
            return connectResultAfterCallback
        }

        override suspend fun disconnect() {
            connected = false
            warning = null
            disconnectCount += 1
        }

        override fun connectionWarning(): String? {
            val current = warning.takeIf { connected }
            onConnectionWarningRead?.also { callback ->
                onConnectionWarningRead = null
                callback()
            }
            return current
        }

        override suspend fun synchronizeClock() {
            clockSyncCount += 1
            nextClockFailure?.let { code ->
                nextClockFailure = null
                warning = code
                throw TransportException(code)
            }
            warning = null
        }

        override suspend fun action(command: ActionCommand) = run {
            synchronizeClock()
            delegate.action(command).also { appliedActions += 1 }
        }
    }
}
