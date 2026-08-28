package ptl.kitsu.app.transport

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareFunApiPolicyTest {
    @Test
    fun enablesFunApiOnlyAtFirmware019OrNewer() {
        assertFalse(FirmwareFunApiPolicy.supportsV1(null))
        assertFalse(FirmwareFunApiPolicy.supportsV1("0.18.9"))
        assertFalse(FirmwareFunApiPolicy.supportsV1("garbage"))
        assertTrue(FirmwareFunApiPolicy.supportsV1("0.19.0"))
        assertTrue(FirmwareFunApiPolicy.supportsV1("0.19.0-test"))
        assertTrue(FirmwareFunApiPolicy.supportsV1("1.0.0"))
    }
}
