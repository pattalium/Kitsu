package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NearbyKitsuPresentationTest {
    @Test fun knownStarterAndEveryPublicWildPackResolveToTheirOwnMonochromePortraits() {
        val cat = nearbyCreaturePresentation(0xFDC79D6FL)

        assertEquals("Cat", cat.name)
        assertEquals(512, cat.bitmap.size)
        assertTrue(cat.known)

        val expected = linkedMapOf(
            0x5CAC86A3L to "Frog",
            0x13793DC7L to "Hamster",
            0x7495DBFBL to "Turtle",
            0x68D9554EL to "Rabbit",
            0x5DF6BE74L to "Hedgehog",
            0xE59408E0L to "Ferret",
            0x29B4B2F7L to "Otter",
            0x69276D0CL to "Axolotl",
            0x2DFB0797L to "Chinchilla",
            0xC163EFEDL to "Raccoon",
            0x374D2540L to "Capybara",
            0x39FC5B1AL to "Sugar Glider",
            0x91A2DE7BL to "Red Panda",
            0xE04EC405L to "Pangolin",
            0x8E0E1B03L to "Tasmanian Devil",
            0x533B9B30L to "Snow Leopard",
            0x86F3BB5DL to "Okapi",
            0x2D1D89AFL to "Shoebill",
            0xA52160C5L to "Cat Girl",
            0xF0F750BDL to "Rabbit Girl",
            0x52A1C03AL to "Deer Girl",
        )
        val portraitKeys = expected.map { (packId, name) ->
            val creature = nearbyCreaturePresentation(packId)
            assertEquals(name, creature.name)
            assertEquals(36, creature.bitmap.size)
            assertTrue(creature.known)
            creature.bitmap.toList()
        }

        assertEquals(21, portraitKeys.distinct().size)
        val ownerPrivateName = "Fox" + " Girl"
        assertTrue(expected.values.none { it.equals(ownerPrivateName, ignoreCase = true) })
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
