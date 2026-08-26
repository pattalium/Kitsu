package ptl.kitsu.app.repository

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.RepeatSource
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.MockKitsuTransport

class OwnerRepositoryMessageConvergenceTest {
    @Test fun initialCacheHydrationCannotOverwriteANewerLiveMessageSnapshot() = runBlocking {
        val stale = message(revision = "1", repeatCount = 0)
        val live = message(revision = "2", repeatCount = 1, sources = listOf(RepeatSource("A1")))
        val cache = BlockingCache(CacheSnapshot(messages = listOf(stale), writtenAt = 1))
        val transport = transport(live, journalRevision = "2")
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope)

        assertTrue(cache.readStarted.await(5, TimeUnit.SECONDS))
        val connect = async { repository.connectAndRefresh(userInitiated = true) }
        delay(100)
        assertEquals(0, transport.connectCount)

        cache.allowRead.countDown()
        withTimeout(5_000) { connect.await() }

        assertEquals(1, transport.connectCount)
        assertEquals(listOf("1"), repository.state.value.messages.map(Message::id))
        assertEquals("2", repository.state.value.messages.single().revision)
        assertEquals(1, repository.state.value.messages.single().repeatCount)
        assertEquals(1, cache.latestWrite?.messages?.single()?.repeatCount)
        scope.cancel()
    }

    @Test fun companionRefreshConvergesZeroToOneByReplacingTheSameBubbleInPlace() = runBlocking {
        val zero = message(revision = "1", repeatCount = 0)
        val transport = transport(zero, journalRevision = "1")
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope)

        repository.connectAndRefresh(userInitiated = true)
        assertEquals(0, repository.state.value.messages.single().repeatCount)
        assertEquals(true, repository.state.value.messages.single().repeatObservationOpen)
        val callsBeforeEvent = transport.messagesCallCount

        transport.mockMessagePage = page(
            message(
                revision = "2",
                repeatCount = 1,
                sources = listOf(RepeatSource("A1")),
            ),
            journalRevision = "2",
        )
        transport.emitRefresh(sequence = 2)
        withTimeout(5_000) {
            while (repository.state.value.messages.singleOrNull()?.revision != "2") delay(10)
        }

        val converged = repository.state.value.messages
        assertTrue(transport.messagesCallCount > callsBeforeEvent)
        assertEquals(1, converged.size)
        assertEquals("1", converged.single().id)
        assertEquals("2", converged.single().revision)
        assertEquals(1, converged.single().repeatCount)
        assertEquals(listOf(RepeatSource("A1")), converged.single().repeatSources)
        assertFalse(converged.single().repeatSourcesTruncated ?: true)
        withTimeout(5_000) {
            while (cache.latestWrite?.messages?.singleOrNull()?.revision != "2") delay(10)
        }
        assertEquals(1, cache.latestWrite?.messages?.single()?.repeatCount)
        scope.cancel()
    }

    private fun repository(
        transport: MockKitsuTransport,
        cache: OwnerCache,
        scope: CoroutineScope,
    ) = OwnerRepository(
        coordinator = ConnectionCoordinator(transport),
        cache = cache,
        credentials = FakeCredentials,
        pairingService = FakePairing,
        scope = scope,
    )

    private fun transport(message: Message, journalRevision: String) = MockKitsuTransport().apply {
        mockStatus = mockStatus.copy(firmwareVersion = "0.16.1-test")
        mockMessagePage = page(message, journalRevision)
    }

    private fun page(message: Message, journalRevision: String) = MessagePage(
        items = listOf(message),
        cursor = message.id,
        hasMore = false,
        journalSession = "9",
        journalRevision = journalRevision,
        protocolVersion = 4,
    )

    private fun message(
        revision: String,
        repeatCount: Int,
        sources: List<RepeatSource> = emptyList(),
    ) = Message(
        id = "1",
        cursor = "1",
        direction = "outbound",
        channel = "0",
        text = "test",
        state = "sent",
        occurredAt = 1,
        revision = revision,
        journalSession = "9",
        route = "flood",
        localTx = "sent",
        deliveryAck = "not_applicable",
        repeatCount = repeatCount,
        repeatObservationOpen = true,
        repeatSources = sources,
        repeatSourcesTruncated = false,
    )

    private class BlockingCache(initial: CacheSnapshot?) : OwnerCache {
        private val initialSnapshot = initial
        val readStarted = CountDownLatch(1)
        val allowRead = CountDownLatch(1)
        @Volatile var latestWrite: CacheSnapshot? = null

        override fun read(): CacheSnapshot? {
            readStarted.countDown()
            check(allowRead.await(5, TimeUnit.SECONDS)) { "cache_read_timeout" }
            return initialSnapshot
        }

        override fun write(snapshot: CacheSnapshot) {
            latestWrite = snapshot
        }

        override fun clear() {
            latestWrite = null
        }
    }

    private object FakeCredentials : CredentialStore {
        private val bonded = BondedCompanion(
            deviceAddress = "00:11:22:33:44:55",
            displayName = "Kitsu",
            controllerIdB64 = "controller",
            controllerRootB64 = "root",
        )

        override suspend fun bondedCompanion() = bonded
        override suspend fun bondedCompanions() = listOf(bonded)
        override suspend fun saveBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun selectBondedCompanion(deviceAddress: String) = bonded
        override suspend fun removeBondedCompanion(deviceAddress: String) = true
        override suspend fun pendingBondedCompanion(): BondedCompanion? = null
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun pendingControllerForgetAddress(): String? = null
        override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) = Unit
    }

    private object FakePairing : ControllerPairingService {
        override suspend fun pairController(
            label: String,
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun finishPendingPairing(
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun repairBluetoothPairing(
            deviceAddress: String,
            onProgress: (BluetoothPairingRepairProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override fun cancelPairing() = Unit
    }
}
