package app.kitsu.mobile

import app.kitsu.mobile.ui.gatewayEnrollmentPresentation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GatewayEnrollmentStatePolicyTest {
    @Test fun stopIsAvailableOnlyBeforePhysicalCommit() {
        val preCommit = listOf(
            MainViewModel.GatewayEnrollmentState.CreatingClaim,
            MainViewModel.GatewayEnrollmentState.WaitingForPhysicalConfirmation(30_000),
        )
        preCommit.forEach { state ->
            assertEquals(GatewayEnrollmentStopDecision.STOP_AND_DISCONNECT, state.stopDecision())
            assertFalse(state.physicalCommitAccepted())
        }

        val committed = listOf(
            MainViewModel.GatewayEnrollmentState.SwitchingToWifi,
            MainViewModel.GatewayEnrollmentState.PollingBackend,
        )
        committed.forEach { state ->
            assertEquals(
                GatewayEnrollmentStopDecision.KEEP_MONITORING_COMMITTED_BOOTSTRAP,
                state.stopDecision(),
            )
            assertTrue(state.physicalCommitAccepted())
            assertFalse(state.canStartNewEnrollment())
        }
    }

    @Test fun committedFailureNeverOffersAReplacementEnrollmentOrFalseCancellationCopy() {
        val state = MainViewModel.GatewayEnrollmentState.Failed(
            code = "backend_poll_not_authorized",
            physicalCommitAccepted = true,
        )
        val presentation = gatewayEnrollmentPresentation(state)

        assertTrue(state.physicalCommitAccepted())
        assertFalse(state.canStartNewEnrollment())
        assertEquals(GatewayEnrollmentStopDecision.NOTHING_TO_STOP, state.stopDecision())
        assertTrue(presentation.title.contains("accepted enrollment"))
        assertTrue(presentation.detail.contains("may still complete"))
        assertFalse(presentation.detail.contains("cancel", ignoreCase = true))
    }

    @Test fun stoppedBeforeConfirmationCanOnlyResumeThroughAnExplicitReconnect() {
        val state = MainViewModel.GatewayEnrollmentState.CancelledBeforePhysicalConfirmation
        val presentation = gatewayEnrollmentPresentation(state)

        assertFalse(state.physicalCommitAccepted())
        assertTrue(state.canStartNewEnrollment())
        assertEquals(GatewayEnrollmentStopDecision.NOTHING_TO_STOP, state.stopDecision())
        assertTrue(presentation.title.contains("before acceptance was reported"))
        assertTrue(presentation.detail.contains("reconnect explicitly"))
        assertTrue(presentation.detail.contains("may still continue"))
    }
}
