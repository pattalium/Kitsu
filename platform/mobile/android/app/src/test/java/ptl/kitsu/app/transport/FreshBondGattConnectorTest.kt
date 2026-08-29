package ptl.kitsu.app.transport

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import ptl.kitsu.app.pairing.PairingException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class FreshBondGattConnectorTest {
    @Test fun freshBondWaitsForAdvertisingBeforeOpeningGatt() = runTest {
        val calls = mutableListOf<String>()

        val opened = FreshBondGattConnector.open(
            freshBond = true,
            initialDevice = "pre-bond",
            awaitAdvertisement = { calls += "advertisement"; "post-bond" },
            connect = { calls += "connect:$it"; ConnectResult.Connected },
        )

        assertEquals(listOf("advertisement", "connect:post-bond"), calls)
        assertEquals("post-bond", opened.device)
        assertEquals(ConnectResult.Connected, opened.result)
    }

    @Test fun status22RescansAndReconnectsExactlyOnce() = runTest {
        var advertisements = 0
        var connections = 0

        val opened = FreshBondGattConnector.open(
            freshBond = true,
            initialDevice = "pre-bond",
            awaitAdvertisement = { "advertisement-${++advertisements}" },
            connect = {
                connections += 1
                if (connections == 1) ConnectResult.Failed("gatt_local_host_terminated")
                else ConnectResult.Connected
            },
        )

        assertEquals(2, advertisements)
        assertEquals(2, connections)
        assertEquals("advertisement-2", opened.device)
        assertEquals(ConnectResult.Connected, opened.result)
    }

    @Test fun repeatedStatus22IsTerminalAfterOneRetry() = runTest {
        var advertisements = 0
        var connections = 0

        val opened = FreshBondGattConnector.open(
            freshBond = true,
            initialDevice = "pre-bond",
            awaitAdvertisement = { "advertisement-${++advertisements}" },
            connect = {
                connections += 1
                ConnectResult.Failed("gatt_local_host_terminated")
            },
        )

        assertEquals(2, advertisements)
        assertEquals(2, connections)
        assertEquals(ConnectResult.Failed("gatt_local_host_terminated"), opened.result)
    }

    @Test fun non22FailureNeverRetries() = runTest {
        var advertisements = 0
        var connections = 0

        val opened = FreshBondGattConnector.open(
            freshBond = true,
            initialDevice = "pre-bond",
            awaitAdvertisement = { "advertisement-${++advertisements}" },
            connect = {
                connections += 1
                ConnectResult.Failed("gatt_status_133")
            },
        )

        assertEquals(1, advertisements)
        assertEquals(1, connections)
        assertEquals(ConnectResult.Failed("gatt_status_133"), opened.result)
    }

    @Test fun anAlreadyBondedAdvertisedDeviceDoesNotEnterFreshBondRecovery() = runTest {
        var advertisements = 0
        var connections = 0

        val opened = FreshBondGattConnector.open(
            freshBond = false,
            initialDevice = "current-advertisement",
            awaitAdvertisement = { advertisements += 1; "unexpected" },
            connect = {
                connections += 1
                ConnectResult.Failed("gatt_local_host_terminated")
            },
        )

        assertEquals(0, advertisements)
        assertEquals(1, connections)
        assertEquals("current-advertisement", opened.device)
    }

    @Test fun missingPostBondAdvertisementFailsBeforeAnyGattOrProtocolWork() = runTest {
        var connections = 0
        val absent = PairingException("pairing_device_absent")

        val failure = runCatching {
            FreshBondGattConnector.open(
                freshBond = true,
                initialDevice = "pre-bond",
                awaitAdvertisement = { throw absent },
                connect = { connections += 1; ConnectResult.Connected },
            )
        }.exceptionOrNull()

        assertSame(absent, failure)
        assertEquals(0, connections)
    }

    @Test fun cancellationDuringPostBondScanPropagatesWithoutOpeningGatt() = runTest {
        var connections = 0
        val cancelled = CancellationException("cancelled")

        val failure = runCatching {
            FreshBondGattConnector.open(
                freshBond = true,
                initialDevice = "pre-bond",
                awaitAdvertisement = { throw cancelled },
                connect = { connections += 1; ConnectResult.Connected },
            )
        }.exceptionOrNull()

        assertSame(cancelled, failure)
        assertEquals(0, connections)
        assertTrue(failure is CancellationException)
    }

    @Test fun cancellationDuringStatus22RescanStopsBeforeASecondGatt() = runTest {
        var advertisements = 0
        var connections = 0
        val cancelled = CancellationException("cancelled")

        val failure = runCatching {
            FreshBondGattConnector.open(
                freshBond = true,
                initialDevice = "pre-bond",
                awaitAdvertisement = {
                    advertisements += 1
                    if (advertisements == 2) throw cancelled
                    "post-bond"
                },
                connect = {
                    connections += 1
                    ConnectResult.Failed("gatt_local_host_terminated")
                },
            )
        }.exceptionOrNull()

        assertSame(cancelled, failure)
        assertEquals(2, advertisements)
        assertEquals(1, connections)
    }
}
