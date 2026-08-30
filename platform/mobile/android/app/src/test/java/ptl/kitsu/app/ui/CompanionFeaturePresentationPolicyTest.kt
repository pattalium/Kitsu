package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
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
    fun focusCountdownUsesFocusPhaseTimeInsteadOfWholeSessionRemaining() {
        val state = focusState(
            phase = FocusPhase.FOCUS,
            elapsedMs = 1_000,
            // Firmware intentionally reports focus + break remaining here.
            remainingMs = 29 * 60_000L + 59_000L,
        )

        val timeline = CompanionFeaturePresentationPolicy.focusTimeline(state)

        assertEquals(
            "Focus phase · 24:59 left",
            CompanionFeaturePresentationPolicy.focusStatus(state),
        )
        assertEquals(1_000L, timeline.phaseElapsedMs)
        assertEquals(24 * 60_000L + 59_000L, timeline.phaseRemainingMs)
        assertTrue(timeline.phaseProgress > 0f)
        assertTrue(timeline.phaseProgress < 0.01f)
    }

    @Test
    fun focusTickerAdvancesLocallyAndBreakStartsAtZeroProgress() {
        val focus = focusState(
            phase = FocusPhase.FOCUS,
            elapsedMs = 10 * 60_000L,
            remainingMs = 20 * 60_000L,
        )
        val advanced = CompanionFeaturePresentationPolicy.focusTimeline(
            focus,
            elapsedSinceSnapshotMs = 5_000L,
        )

        assertEquals(10 * 60_000L + 5_000L, advanced.phaseElapsedMs)
        assertEquals(14 * 60_000L + 55_000L, advanced.phaseRemainingMs)
        assertEquals(
            "Focus phase · 14:55 left",
            CompanionFeaturePresentationPolicy.focusStatus(focus, 5_000L),
        )

        val breakState = focusState(
            phase = FocusPhase.BREAK,
            elapsedMs = 25 * 60_000L,
            remainingMs = 5 * 60_000L,
            prompt = FocusPrompt("FOCUS COMPLETE", "TRY PULSE BREATHING", true),
        )
        val breakTimeline = CompanionFeaturePresentationPolicy.focusTimeline(breakState)
        assertEquals(
            "Break phase · 5:00 left",
            CompanionFeaturePresentationPolicy.focusStatus(breakState),
        )
        assertEquals(0f, breakTimeline.phaseProgress, 0f)
        assertEquals(5 * 60_000L, breakTimeline.phaseRemainingMs)
        assertTrue(breakTimeline.sessionProgress > 0.8f)
        assertTrue(breakTimeline.sessionProgress < 0.84f)
    }

    private fun focusState(
        phase: FocusPhase,
        elapsedMs: Long,
        remainingMs: Long,
        prompt: FocusPrompt = FocusPrompt("FOCUS TIME", "STAY WITH IT", false),
    ) = FocusSessionState(
        ok = true,
        schema = 1,
        phase = phase,
        completion = FocusCompletion.NONE,
        sessionId = 7,
        focusMinutes = 25,
        breakMinutes = 5,
        elapsedMs = elapsedMs,
        remainingMs = remainingMs,
        sequence = 1,
        prompt = prompt,
    )
}
