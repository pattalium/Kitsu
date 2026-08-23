package ptl.kitsu.app

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MessageSubmissionCoordinatorTest {
    @Test fun rapidDoubleTapRunsExactlyOneTransportAction() = runTest {
        val coordinator = MessageSubmissionCoordinator()
        val releaseReceipt = CompletableDeferred<Unit>()
        var transportActions = 0

        val first = async {
            coordinator.runOnce {
                transportActions += 1
                releaseReceipt.await()
            }
        }
        testScheduler.runCurrent()
        assertTrue(coordinator.inFlight.value)

        val second = async {
            coordinator.runOnce { transportActions += 1 }
        }
        testScheduler.runCurrent()

        assertFalse(second.await())
        assertEquals(1, transportActions)
        releaseReceipt.complete(Unit)
        assertTrue(first.await())
        assertFalse(coordinator.inFlight.value)
    }

    @Test fun acceptedReceiptReleasesComposerWhileJournalRefreshIsStillPending() = runTest {
        val coordinator = MessageSubmissionCoordinator()
        val journalRefresh = CompletableDeferred<Unit>()
        var bodyCleared = false

        val accepted = coordinator.runOnce {
            // This block models the accepted queued receipt. Repository journal
            // synchronization is deliberately launched after, not awaited by it.
            bodyCleared = true
            backgroundScope.launch { journalRefresh.await() }
        }

        assertTrue(accepted)
        assertTrue(bodyCleared)
        assertFalse(coordinator.inFlight.value)
        assertFalse(journalRefresh.isCompleted)
    }
}
