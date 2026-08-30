package ptl.kitsu.app.transport

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AuthenticatedResponseQuietPeriodTest {
    @Test fun nextRequestWaitsForFullQuietPeriodAfterResponse() = runTest {
        val mutex = Mutex()
        val firstResponse = CompletableDeferred<Unit>()
        val requestStarts = mutableListOf<Long>()

        val first = async {
            mutex.withResponseQuietPeriod {
                requestStarts += testScheduler.currentTime
                firstResponse.await()
                "first"
            }
        }
        testScheduler.runCurrent()

        val second = async {
            mutex.withResponseQuietPeriod {
                requestStarts += testScheduler.currentTime
                "second"
            }
        }
        testScheduler.runCurrent()
        firstResponse.complete(Unit)
        testScheduler.runCurrent()

        assertEquals(listOf(0L), requestStarts)
        assertFalse(first.isCompleted)
        assertFalse(second.isCompleted)

        testScheduler.advanceTimeBy(AUTHENTICATED_RESPONSE_QUIET_PERIOD_MILLIS - 1L)
        testScheduler.runCurrent()
        assertEquals(listOf(0L), requestStarts)

        testScheduler.advanceTimeBy(1L)
        testScheduler.runCurrent()
        assertEquals(
            listOf(0L, AUTHENTICATED_RESPONSE_QUIET_PERIOD_MILLIS),
            requestStarts,
        )
        assertTrue(first.isCompleted)
        assertFalse(second.isCompleted)

        testScheduler.advanceTimeBy(AUTHENTICATED_RESPONSE_QUIET_PERIOD_MILLIS)
        testScheduler.runCurrent()
        assertEquals("first", first.await())
        assertEquals("second", second.await())
    }
}
