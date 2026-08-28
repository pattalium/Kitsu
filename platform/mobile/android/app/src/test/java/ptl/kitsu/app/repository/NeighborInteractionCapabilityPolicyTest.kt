package ptl.kitsu.app.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import ptl.kitsu.app.model.NeighborInteractionKind

class NeighborInteractionCapabilityPolicyTest {
    @Test
    fun onlyAnAuthenticatedAdvertisedActionIsEligibleForTransport() {
        val legacyFirmware = setOf(NeighborInteractionKind.PET)
        assertNull(
            NeighborInteractionCapabilityPolicy.validationError(
                legacyFirmware,
                NeighborInteractionKind.PET,
            ),
        )
        assertEquals(
            "neighbor_action_unsupported",
            NeighborInteractionCapabilityPolicy.validationError(
                legacyFirmware,
                NeighborInteractionKind.GREET,
            ),
        )
        assertEquals(
            "neighbor_action_unsupported",
            NeighborInteractionCapabilityPolicy.validationError(
                emptySet(),
                NeighborInteractionKind.PLAY,
            ),
        )
        assertEquals(
            "neighbor_action_unsupported",
            NeighborInteractionCapabilityPolicy.validationError(
                legacyFirmware,
                NeighborInteractionKind.GIFT,
            ),
        )

        val expandedFirmware = NeighborInteractionKind.entries.toSet()
        NeighborInteractionKind.entries.forEach { kind ->
            assertNull(NeighborInteractionCapabilityPolicy.validationError(expandedFirmware, kind))
        }
    }
}
