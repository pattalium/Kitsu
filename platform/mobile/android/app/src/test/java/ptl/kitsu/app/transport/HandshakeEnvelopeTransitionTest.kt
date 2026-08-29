package ptl.kitsu.app.transport

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HandshakeEnvelopeTransitionTest {
    @Test fun encryptedRefreshAfterCompletedDeviceOkIsBufferedAndDrainedInOrder() {
        val transition = HandshakeEnvelopeTransition()
        val waiting = CompletableDeferred<ByteArray>()
        transition.begin(generation = 7)

        val deviceOk = "device_ok".toByteArray()
        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.HANDSHAKE_COMPLETED,
            transition.accept(waiting, generation = 7, payload = deviceOk),
        )
        assertArrayEquals(deviceOk, runBlocking { waiting.await() })

        val refreshSequenceOne = "encrypted-refresh-sequence-1".toByteArray()
        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.BUFFERED,
            // CompletableDeferred.complete() returns false here; this is the
            // callback race that previously dropped device sequence one.
            transition.accept(waiting, generation = 7, payload = refreshSequenceOne),
        )
        val batch = transition.drainOrFinish(7) as HandshakeEnvelopeTransition.DrainResult.Batch
        assertArrayEquals(refreshSequenceOne, batch.payloads.single())
        assertEquals(
            HandshakeEnvelopeTransition.DrainResult.Finished,
            transition.drainOrFinish(7),
        )
        assertFalse(transition.isActive(7))
    }

    @Test fun nullHandshakeDeferredTimingStillBuffersUntilSessionPublication() {
        val transition = HandshakeEnvelopeTransition()
        transition.begin(generation = 11)

        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.BUFFERED,
            transition.accept(null, 11, "event".toByteArray()),
        )
        assertTrue(transition.isActive(11))
    }

    @Test fun transitionIsBoundedAndRejectsStaleGattGenerations() {
        val transition = HandshakeEnvelopeTransition(maxFrames = 1, maxBytes = 8)
        transition.begin(generation = 3)

        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.STALE_GENERATION,
            transition.accept(null, 2, byteArrayOf(1)),
        )
        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.BUFFERED,
            transition.accept(null, 3, byteArrayOf(1, 2, 3, 4)),
        )
        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.OVERFLOW,
            transition.accept(null, 3, byteArrayOf(5)),
        )
        assertEquals(
            HandshakeEnvelopeTransition.DrainResult.StaleGeneration,
            transition.drainOrFinish(4),
        )
    }

    @Test fun staleGenerationCannotCompleteTheCurrentLiveHandshakeWaiter() {
        val transition = HandshakeEnvelopeTransition()
        val currentWaiter = CompletableDeferred<ByteArray>()
        transition.begin(generation = 3)

        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.STALE_GENERATION,
            transition.accept(currentWaiter, generation = 2, payload = "stale".toByteArray()),
        )
        assertFalse(currentWaiter.isCompleted)
        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.HANDSHAKE_COMPLETED,
            transition.accept(currentWaiter, generation = 3, payload = "current".toByteArray()),
        )
        assertArrayEquals("current".toByteArray(), runBlocking { currentWaiter.await() })
    }

    @Test fun cancelledWaiterBuffersForTheLiveGenerationAndClearInvalidatesIt() {
        val transition = HandshakeEnvelopeTransition()
        val cancelledWaiter = CompletableDeferred<ByteArray>().also { it.cancel() }
        transition.begin(generation = 9)

        assertEquals(
            HandshakeEnvelopeTransition.AcceptResult.BUFFERED,
            transition.accept(cancelledWaiter, generation = 9, payload = "event".toByteArray()),
        )
        transition.clear()
        assertFalse(transition.isActive(9))
        assertEquals(
            HandshakeEnvelopeTransition.DrainResult.StaleGeneration,
            transition.drainOrFinish(9),
        )
    }
}
