package ptl.kitsu.app.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareMessageApiPolicyTest {
    @Test fun messageOperationIsSelectedOnlyFromTheAuthenticatedFirmwareVersion() {
        listOf(null, "", "0.13.9", "0.12.0", "garbage").forEach {
            assertEquals(1, FirmwareMessageApiPolicy.protocolVersion(it))
        }
        listOf(
            "0.14.0", "0.14.1", "0.14.0-preview", "0.15.0", "0.15.1-preview", "0.15.99",
        ).forEach {
            assertEquals(2, FirmwareMessageApiPolicy.protocolVersion(it))
        }
        listOf("0.16.0", "0.16.0-preview").forEach {
            assertEquals(3, FirmwareMessageApiPolicy.protocolVersion(it))
        }
        listOf("0.16.1", "0.16.1-preview", "0.17.0", "1.0.0").forEach {
            assertEquals(4, FirmwareMessageApiPolicy.protocolVersion(it))
        }
        assertEquals("messages.get", FirmwareMessageApiPolicy.operation(1))
        assertEquals("messages.get.v2", FirmwareMessageApiPolicy.operation(2))
        assertEquals("messages.get.v3", FirmwareMessageApiPolicy.operation(3))
        assertEquals("messages.get.v4", FirmwareMessageApiPolicy.operation(4))
        assertTrue(runCatching { FirmwareMessageApiPolicy.operation(0) }.isFailure)
        assertTrue(runCatching { FirmwareMessageApiPolicy.operation(5) }.isFailure)
    }

    @Test fun markReadRequiresDistinctAuthenticatedZeroFifteenCapability() {
        listOf(null, "", "0.14.0", "0.14.99", "garbage").forEach {
            assertFalse(FirmwareMessageApiPolicy.supportsMarkRead(it))
        }
        listOf("0.15.0", "0.15.1-preview", "0.16.0", "1.0.0").forEach {
            assertTrue(FirmwareMessageApiPolicy.supportsMarkRead(it))
        }
    }

    @Test fun markReadRequestRequiresCanonicalSessionAndUniqueBoundedIds() {
        FirmwareMessageApiPolicy.requireMarkReadRequest("7", listOf("1", "4294967295"))

        listOf(
            { FirmwareMessageApiPolicy.requireMarkReadRequest("0", listOf("1")) },
            { FirmwareMessageApiPolicy.requireMarkReadRequest("07", listOf("1")) },
            { FirmwareMessageApiPolicy.requireMarkReadRequest("7", emptyList()) },
            { FirmwareMessageApiPolicy.requireMarkReadRequest("7", listOf("1", "1")) },
            { FirmwareMessageApiPolicy.requireMarkReadRequest("7", listOf("01")) },
            { FirmwareMessageApiPolicy.requireMarkReadRequest("7", (1..25).map(Int::toString)) },
        ).forEach { invalid ->
            assertTrue(runCatching(invalid).exceptionOrNull() is IllegalArgumentException)
        }
    }
}
