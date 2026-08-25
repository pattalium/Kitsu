package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class EncounterUnlockPresentationTest {
    @Test
    fun maskNeverReturnsTheFullSavedCode() {
        val code = "KITSU-SECRET-1234"
        val masked = maskEncounterCode(code)

        assertEquals("KITS••••••1234", masked)
        assertFalse(masked.contains("SECRET"))
        assertFalse(masked == code)
    }
}
