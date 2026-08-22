package app.kitsu.mobile.connection

import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.MockKitsuTransport
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
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
        val coordinator = ConnectionCoordinator(MockKitsuTransport())
        coordinator.connect(userInitiated = true)
        coordinator.onDirectTransportDisconnected("gatt_disconnected")
        assertFalse(coordinator.state.value.connected)
        assertEquals("gatt_disconnected", coordinator.state.value.detail)
    }
}
