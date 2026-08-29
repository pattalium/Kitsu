package ptl.kitsu.app.transport

import java.util.ArrayDeque
import kotlinx.coroutines.CompletableDeferred

/**
 * Bridges the callback-sized gap between device_ok delivery and publication of
 * the derived authenticated envelope session.
 *
 * Firmware may immediately follow device_ok with an encrypted refresh event.
 * Every such frame stays generation-bound and ordered here until the session is
 * installed. The queue is deliberately tiny and fail-closed.
 */
internal class HandshakeEnvelopeTransition(
    private val maxFrames: Int = 4,
    private val maxBytes: Int = MAX_GATT_JSON_BYTES * 2,
) {
    enum class AcceptResult {
        HANDSHAKE_COMPLETED,
        BUFFERED,
        INACTIVE,
        STALE_GENERATION,
        OVERFLOW,
    }

    sealed interface DrainResult {
        data class Batch(val payloads: List<ByteArray>) : DrainResult
        data object Finished : DrainResult
        data object StaleGeneration : DrainResult
    }

    private var generation: Long? = null
    private val payloads = ArrayDeque<ByteArray>()
    private var bufferedBytes = 0

    init {
        require(maxFrames > 0) { "transition_frame_capacity_required" }
        require(maxBytes > 0) { "transition_byte_capacity_required" }
    }

    @Synchronized
    fun begin(generation: Long) {
        clearLocked()
        this.generation = generation
    }

    /** Claims a live handshake response, or buffers a frame after that response won its race. */
    @Synchronized
    fun accept(
        waiting: CompletableDeferred<ByteArray>?,
        generation: Long,
        payload: ByteArray,
    ): AcceptResult {
        val active = this.generation ?: return AcceptResult.INACTIVE
        if (active != generation) return AcceptResult.STALE_GENERATION
        if (waiting != null) {
            val claimed = if (payload.size > CapabilityHandshake.MAX_HANDSHAKE_BYTES) {
                waiting.completeExceptionally(HandshakeException("invalid_handshake_frame"))
            } else {
                waiting.complete(payload)
            }
            if (claimed) return AcceptResult.HANDSHAKE_COMPLETED
        }
        return bufferLocked(payload)
    }

    private fun bufferLocked(payload: ByteArray): AcceptResult {
        if (payload.isEmpty() || payload.size > MAX_GATT_JSON_BYTES ||
            payloads.size >= maxFrames || bufferedBytes > maxBytes - payload.size
        ) {
            return AcceptResult.OVERFLOW
        }
        payloads.addLast(payload.copyOf())
        bufferedBytes += payload.size
        return AcceptResult.BUFFERED
    }

    /**
     * Removes the current ordered batch while leaving the transition active.
     * Once a caller observes Finished, deactivation and the empty check happened
     * atomically, so later callbacks may safely decode against the live session.
     */
    @Synchronized
    fun drainOrFinish(generation: Long): DrainResult {
        val active = this.generation ?: return DrainResult.StaleGeneration
        if (active != generation) return DrainResult.StaleGeneration
        if (payloads.isEmpty()) {
            this.generation = null
            bufferedBytes = 0
            return DrainResult.Finished
        }
        val batch = buildList(payloads.size) {
            while (payloads.isNotEmpty()) add(payloads.removeFirst())
        }
        bufferedBytes = 0
        return DrainResult.Batch(batch)
    }

    @Synchronized
    fun clear() {
        clearLocked()
    }

    @Synchronized
    fun isActive(generation: Long): Boolean = this.generation == generation

    private fun clearLocked() {
        payloads.forEach { it.fill(0) }
        payloads.clear()
        bufferedBytes = 0
        generation = null
    }

}
