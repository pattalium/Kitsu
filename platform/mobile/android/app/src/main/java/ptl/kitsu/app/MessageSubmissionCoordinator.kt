package ptl.kitsu.app

import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Allows one logical send action until its accepted/rejected receipt returns. */
internal class MessageSubmissionCoordinator {
    private val active = AtomicBoolean(false)
    private val mutableInFlight = MutableStateFlow(false)
    val inFlight: StateFlow<Boolean> = mutableInFlight.asStateFlow()

    suspend fun runOnce(block: suspend () -> Unit): Boolean {
        if (!active.compareAndSet(false, true)) return false
        mutableInFlight.value = true
        return try {
            block()
            true
        } finally {
            mutableInFlight.value = false
            active.set(false)
        }
    }
}
