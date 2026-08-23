package ptl.kitsu.app.repository

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

class OwnerConnectRefreshOrderTest {
    @Test fun eventArrivingDuringInitialRefreshHasAnActiveSubscriber() = runTest {
        val events = MutableSharedFlow<String>(replay = 0, extraBufferCapacity = 1)
        val observed = mutableListOf<String>()
        var collector: Job? = null

        OwnerConnectRefreshOrder.run(
            subscribe = {
                collector = backgroundScope.launch(start = CoroutineStart.UNDISPATCHED) {
                    events.collect(observed::add)
                }
            },
            initialRefresh = {
                events.emit("message-arrived-during-refresh")
                testScheduler.runCurrent()
            },
        )

        assertEquals(listOf("message-arrived-during-refresh"), observed)
        collector?.cancel()
    }
}
