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

        assertFalse(FirmwareEncounterApiPolicy.supportsCatalogV1(null))
        assertFalse(FirmwareEncounterApiPolicy.supportsCatalogV1("0.17.9"))
        assertTrue(FirmwareEncounterApiPolicy.supportsCatalogV1("0.18.0"))
        assertTrue(FirmwareEncounterApiPolicy.supportsCatalogV1("0.18.0-test"))
        assertTrue(FirmwareEncounterApiPolicy.supportsCatalogV1("1.0.0"))

        assertFalse(FirmwareEncounterApiPolicy.supportsDiscoveryV1(null))
        assertFalse(FirmwareEncounterApiPolicy.supportsDiscoveryV1("0.20.1"))
        assertFalse(FirmwareEncounterApiPolicy.supportsDiscoveryV1("0.19.99"))
        assertTrue(FirmwareEncounterApiPolicy.supportsDiscoveryV1("0.20.2"))
        assertTrue(FirmwareEncounterApiPolicy.supportsDiscoveryV1("0.20.2-test"))
        assertTrue(FirmwareEncounterApiPolicy.supportsDiscoveryV1("0.21.0"))
        assertTrue(FirmwareEncounterApiPolicy.supportsDiscoveryV1("1.0.0"))
    }
}
