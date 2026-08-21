package app.kitsu.mobile.transport

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectProvisioningPlanTest {
    @Test fun wifiWriteFollowsSuccessfulClockSync() = runTest {
        val events = mutableListOf<String>()
        val result = DirectProvisioningPlan.execute(
            writeOperation = "wifi.configure",
            synchronizeClock = { events += "clock.sync" },
            write = { operation ->
                events += operation
                "stored"
            },
        )

        assertEquals("stored", result)
        assertEquals(listOf("clock.sync", "wifi.configure"), events)
    }

    @Test fun gatewayWriteFollowsSuccessfulClockSync() = runTest {
        val events = mutableListOf<String>()
        DirectProvisioningPlan.execute(
            writeOperation = "gateway.configure",
            synchronizeClock = { events += "clock.sync" },
            write = { operation -> events += operation },
        )

        assertEquals(listOf("clock.sync", "gateway.configure"), events)
    }

    @Test fun failedClockSyncPreventsSecretBearingWrite() = runTest {
        var writeAttempted = false
        val failure = runCatching {
            DirectProvisioningPlan.execute(
                writeOperation = "wifi.configure",
                synchronizeClock = { throw TransportException("clock_sync_failed") },
                write = {
                    writeAttempted = true
                },
            )
        }.exceptionOrNull()

        assertTrue(failure is TransportException)
        assertEquals("clock_sync_failed", (failure as TransportException).code)
        assertEquals(false, writeAttempted)
    }

    @Test fun enrollmentBeginFollowsSuccessfulClockSync() = runTest {
        val events = mutableListOf<String>()
        DirectProvisioningPlan.execute(
            writeOperation = "gateway.enroll.begin",
            synchronizeClock = { events += "clock.sync" },
            write = { events += it },
        )

        assertEquals(listOf("clock.sync", "gateway.enroll.begin"), events)
    }

    @Test fun unknownProvisioningOperationIsRejectedBeforeClockOrWrite() = runTest {
        val events = mutableListOf<String>()
        val failure = runCatching {
            DirectProvisioningPlan.execute(
                writeOperation = "credential.export",
                synchronizeClock = { events += "clock.sync" },
                write = { events += it },
            )
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
        assertTrue(events.isEmpty())
    }
}
