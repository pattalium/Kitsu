package ptl.kitsu.app.transport

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwarePetFeatureApiPolicyTest {
    @Test
    fun doesNotProbePetApisBeforeTheirSharedFirmwareRelease() {
        listOf(
            null,
            "",
            "garbage",
            "0.20",
            "v0.20.4",
            "0.19.99",
            "0.20.3",
            "0.20.3-test",
            "999999999999999999999999.20.4",
        ).forEach { version ->
            assertFalse("unexpected support for $version", FirmwarePetFeatureApiPolicy.supportsV1(version))
        }
    }

    @Test
    fun enablesPetApisAt0204AndSemanticSuccessors() {
        listOf(
            "0.20.4",
            "0.20.4-test",
            "0.20.4+build.1",
            "0.20.5",
            "0.21.0",
            "1.0.0",
        ).forEach { version ->
            assertTrue("expected support for $version", FirmwarePetFeatureApiPolicy.supportsV1(version))
        }
    }
}
