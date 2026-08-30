package ptl.kitsu.app

import java.util.Base64
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.security.MessageDraftRecord
import ptl.kitsu.app.security.MessageDraftStore

@OptIn(ExperimentalCoroutinesApi::class)
class MessageDraftCoordinatorTest {
    private val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun editsAreVisibleImmediatelyAndEncryptedStoreWritesAreCoalesced() = runTest {
        val store = FakeDraftStore()
        val dispatcher = StandardTestDispatcher(testScheduler)
        val coordinator = MessageDraftCoordinator(
            store = store,
            scope = backgroundScope,
            ioContext = dispatcher,
            persistDelayMillis = 200,
            nowMillis = { 10 },
        )
        runCurrent()

        coordinator.update("AA", "direct:$peer", "h")
        coordinator.update("AA", "direct:$peer", "hello")
        assertEquals(mapOf("direct:$peer" to "hello"), coordinator.forDevice("aa"))
        assertTrue(store.writes.isEmpty())

        advanceTimeBy(200)
        runCurrent()
        assertEquals("hello", store.writes.single().single().text)
    }

    @Test fun rejectedSendKeepsDraftAndAcceptedSendClearsOnlyItsBinding() = runTest {
        val store = FakeDraftStore()
        val dispatcher = StandardTestDispatcher(testScheduler)
        val coordinator = MessageDraftCoordinator(
            store = store,
            scope = backgroundScope,
            ioContext = dispatcher,
            persistDelayMillis = 0,
            nowMillis = { 20 },
        )
        runCurrent()
        coordinator.update("AA", "direct:$peer", "keep after rejection")
        coordinator.update("AA", "channel:0", "other thread")
        runCurrent()

        // MainViewModel invokes the empty-body callback only after repository.perform succeeds.
        val rejected = Result.failure<Unit>(IllegalStateException("rejected"))
        if (rejected.isSuccess) coordinator.update("AA", "direct:$peer", "")
        assertEquals("keep after rejection", coordinator.forDevice("AA")["direct:$peer"])

        val accepted = Result.success(Unit)
        if (accepted.isSuccess) coordinator.update("AA", "direct:$peer", "")
        runCurrent()
        assertEquals(mapOf("channel:0" to "other thread"), coordinator.forDevice("AA"))
        assertEquals(mapOf("channel:0" to "other thread"), store.writes.last().associate { it.threadKey to it.text })
    }

    @Test fun firstEditWaitsForInitialReadAndDoesNotEraseOtherDrafts() = runTest {
        val readGate = CompletableDeferred<Unit>()
        val otherPeer = Base64.getUrlEncoder().withoutPadding().encodeToString(
            ByteArray(32) { index -> (index + 1).toByte() },
        )
        val store = FakeDraftStore(
            stored = listOf(MessageDraftRecord("AA", "direct:$otherPeer", "already stored", 1)),
            readGate = readGate,
        )
        val coordinator = MessageDraftCoordinator(
            store = store,
            scope = backgroundScope,
            ioContext = StandardTestDispatcher(testScheduler),
            persistDelayMillis = 0,
            nowMillis = { 2 },
        )
        runCurrent()

        coordinator.update("AA", "direct:$peer", "new local edit")
        runCurrent()
        assertTrue(store.writes.isEmpty())

        readGate.complete(Unit)
        runCurrent()
        assertEquals(
            mapOf(
                "direct:$otherPeer" to "already stored",
                "direct:$peer" to "new local edit",
            ),
            store.writes.single().associate { it.threadKey to it.text },
        )
    }

    private class FakeDraftStore(
        private var stored: List<MessageDraftRecord> = emptyList(),
        private val readGate: CompletableDeferred<Unit>? = null,
    ) : MessageDraftStore {
        val writes = mutableListOf<List<MessageDraftRecord>>()

        override suspend fun read(): List<MessageDraftRecord> {
            readGate?.await()
            return stored
        }

        override suspend fun write(records: List<MessageDraftRecord>) {
            stored = records.toList()
            writes += stored
        }
    }
}
