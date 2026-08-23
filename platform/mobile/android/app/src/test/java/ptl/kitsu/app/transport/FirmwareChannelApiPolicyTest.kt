package ptl.kitsu.app.transport

import org.junit.Assert.assertEquals
import org.junit.Test

class FirmwareChannelApiPolicyTest {
    @Test fun v2IsSelectedOnlyFromAuthenticatedFirmwareAtLeast0164() {
        listOf(null, "", "0.16.3", "garbage").forEach { version ->
            assertEquals(1, FirmwareChannelApiPolicy.protocolVersion(version))
        }
        listOf("0.16.4", "0.16.4-rc1", "0.17.0", "1.0.0").forEach { version ->
            assertEquals(2, FirmwareChannelApiPolicy.protocolVersion(version))
        }
    }

    @Test fun eachProtocolVersionBindsToOneExactOperation() {
        assertEquals("channels.get", FirmwareChannelApiPolicy.operation(1))
        assertEquals("channels.get.v2", FirmwareChannelApiPolicy.operation(2))
    }
}
