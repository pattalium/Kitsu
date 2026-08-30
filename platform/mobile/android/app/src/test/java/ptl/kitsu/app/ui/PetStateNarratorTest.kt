package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.NeedLevels

class PetStateNarratorTest {
    @Test
    fun describesVisiblePetStateWithoutMeshOrInventoryLanguage() {
        val status = KitsuStatus(
            deviceId = "KT1234",
            companionName = "Mochi",
            mood = "CURIOUS",
            needs = NeedLevels(energy = 70, curiosity = 55, affection = 80),
            updatedAt = 1,
        )

        val description = PetStateNarrator.describe(status, null, null)

        assertEquals(
            "Mochi feels curious. Energy 70 percent, curiosity 55 percent, affection 80 percent.",
            description,
        )
        assertFalse(description.contains("mesh", ignoreCase = true))
        assertFalse(description.contains("inventory", ignoreCase = true))
    }
}
