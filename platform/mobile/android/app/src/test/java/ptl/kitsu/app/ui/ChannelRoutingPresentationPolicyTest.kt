package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.ChannelRegionScope

class ChannelRoutingPresentationPolicyTest {
    @Test fun legacyDoesNotClaimThatEveryRepeaterParticipates() {
        val presentation = ChannelRoutingPresentationPolicy.present(null)

        assertEquals("Legacy", presentation.label)
        assertTrue(presentation.detail.contains("depends on each repeater's channel configuration"))
    }

    @Test fun euScopeNamesTheAllowlistBoundary() {
        val presentation = ChannelRoutingPresentationPolicy.present(ChannelRegionScope.EU)

        assertEquals("Scoped #EU", presentation.label)
        assertTrue(presentation.detail.contains("only repeaters configured to allow #EU can participate"))
    }
}
