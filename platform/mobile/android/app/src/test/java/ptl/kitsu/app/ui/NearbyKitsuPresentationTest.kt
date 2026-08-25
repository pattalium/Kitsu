package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NearbyKitsuPresentationTest {
    @Test fun knownStarterAndWildPacksResolveToTheirOwnMonochromePortraits() {
        val cat = nearbyCreaturePresentation(0xFDC79D6FL)
        val frog = nearbyCreaturePresentation(0x5CAC86A3L)

        assertEquals("Cat", cat.name)
        assertEquals(512, cat.bitmap.size)
        assertTrue(cat.known)
        assertEquals("Frog", frog.name)
        assertEquals(36, frog.bitmap.size)
        assertTrue(frog.known)
    }

    @Test fun unknownPackUsesExplicitUnknownPortraitWithoutGuessingAnIdentity() {
        val unknown = nearbyCreaturePresentation(0x12345678L)

        assertEquals("Unknown Kitsu", unknown.name)
        assertFalse(unknown.known)
        assertEquals(36, unknown.bitmap.size)
    }

    @Test fun radioMetadataLabelsRemainCompactAndHumanReadable() {
        assertEquals("Listening", nearbyMoodLabel(2))
        assertEquals("Ascended", nearbyStageLabel(4))
        assertEquals("just now", nearbyLastSeenLabel(4_999))
        assertEquals("59s ago", nearbyLastSeenLabel(59_999))
        assertEquals("2m ago", nearbyLastSeenLabel(120_000))
    }
}
