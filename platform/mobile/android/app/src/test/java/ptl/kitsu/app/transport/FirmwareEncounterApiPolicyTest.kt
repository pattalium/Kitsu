package ptl.kitsu.app.transport

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareEncounterApiPolicyTest {
    @Test fun encounterOperationsAreSelectedWithoutProbingOlderFirmware() {
        assertFalse(FirmwareEncounterApiPolicy.supportsV1(null))
        assertFalse(FirmwareEncounterApiPolicy.supportsV1("0.16.5"))
        assertFalse(FirmwareEncounterApiPolicy.supportsV1("garbage"))
        assertTrue(FirmwareEncounterApiPolicy.supportsV1("0.17.0"))
        assertTrue(FirmwareEncounterApiPolicy.supportsV1("0.17.0-test"))
        assertTrue(FirmwareEncounterApiPolicy.supportsV1("1.0.0"))
    }
}
