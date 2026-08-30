package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.FocusCompletion
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusPrompt
import ptl.kitsu.app.model.FocusSessionState

class CompanionFeaturePresentationPolicyTest {
    @Test
    fun everyCompanionActionHasPlainNonInventoryCopy() {
        val labels = CompanionAction.entries.map(CompanionFeaturePresentationPolicy::actionLabel)

        assertEquals(CompanionAction.entries.size, labels.distinct().size)
        labels.forEach { label ->
            val normalized = label.lowercase()
            assertFalse(normalized.contains("inventory"))
            assertFalse(normalized.contains("item"))
            assertFalse(normalized.contains("gift"))
            assertFalse(normalized.contains("reward"))
        }
        assertEquals("Spend time together", CompanionFeaturePresentationPolicy.actionLabel(CompanionAction.GIFT))
    }

    @Test
    fun focusCountdownIsReadableAndNeverNegative() {
        val state = FocusSessionState(
            ok = true,
            schema = 1,
            phase = FocusPhase.FOCUS,
            completion = FocusCompletion.NONE,
            sessionId = 7,
            focusMinutes = 25,
            breakMinutes = 5,
            elapsedMs = 1_000,
            remainingMs = -1,
            sequence = 1,
            prompt = FocusPrompt("", "", false),
        )

        assertEquals("Focusing · 0:00 left", CompanionFeaturePresentationPolicy.focusStatus(state))
    }
}
