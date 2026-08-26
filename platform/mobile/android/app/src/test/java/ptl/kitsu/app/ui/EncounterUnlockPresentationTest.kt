package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class EncounterUnlockPresentationTest {
    @Test
    fun maskNeverReturnsTheFullSavedCode() {
        val code = "K8-ABCDE-FGHJK-MNPQR"
        val masked = maskEncounterCode(code)

        assertEquals("K8-A••••••NPQR", masked)
        assertFalse(masked.contains("FGHJK"))
        assertFalse(masked == code)
    }
}
