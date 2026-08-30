package ptl.kitsu.app.accessibility

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.WalkPhase

class PetSpeechPolicyTest {
    @Test
    fun stateAndPromptTextAreCleanedAndUtf8Bounded() {
        assertEquals(
            "Kitsu feels calm and ready.",
            PetSpeechPolicy.prepare(
                PetSpeechCue(PetSpeechKind.STATE, "  Kitsu\u0000 feels\n calm\tand ready.  "),
            ),
        )
        val bounded = PetSpeechPolicy.prepare(
            PetSpeechCue(PetSpeechKind.CHECK_IN, "🦊".repeat(200)),
        )!!
        assertTrue(bounded.toByteArray(Charsets.UTF_8).size <= PetSpeechPolicy.MAX_UTF8_BYTES)
    }

    @Test
    fun blankTextNeverReachesTheSpeechEngine() {
        assertNull(PetSpeechPolicy.prepare(PetSpeechCue(PetSpeechKind.WALK, " \n\t ")))
    }

    @Test
    fun optInTransitionCuesOnlyFireWhenACompanionMomentActuallyChanges() {
        val idle = PetAccessibilityMoment(
            CompanionRequestState.NONE,
            FocusPhase.FOCUS,
            WalkPhase.ACTIVE,
        )
        assertEquals(
            PetHapticCue.CHECK_IN,
            PetAccessibilityTransitionPolicy.hapticCue(
                idle,
                idle.copy(requestState = CompanionRequestState.PENDING),
            ),
        )
        assertEquals(
            PetHapticCue.FOCUS_COMPLETE,
            PetAccessibilityTransitionPolicy.hapticCue(
                idle,
                idle.copy(focusPhase = FocusPhase.COMPLETED),
            ),
        )
        assertEquals(
            PetHapticCue.WALK_RETURNED,
            PetAccessibilityTransitionPolicy.hapticCue(
                idle,
                idle.copy(walkPhase = WalkPhase.RETURNED),
            ),
        )
        assertNull(PetAccessibilityTransitionPolicy.hapticCue(idle, idle))
    }
}
