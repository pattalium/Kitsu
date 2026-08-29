package ptl.kitsu.app.repository

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.EncounterDiscoveryPage
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.ENCOUNTER_NEIGHBORS_SCHEMA
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.NearbyKitsuPage
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
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
        assertEquals(null, repository.state.value.errorCode)
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

    @Test fun companionRefreshAndReconnectKeepNoCodeDiscoveryCurrentAndOfflineSafe() = runBlocking {
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(deviceId = "KT0001", firmwareVersion = "0.20.2-test")
            mockEncounterDiscoveryPage = discoveryPage("Turtle", 1, "mesh_advert_rx")
            mockMessagePage = page(message("1", 0), "1")
        }
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope)

        repository.connectAndRefresh(userInitiated = true)
        assertEquals(null, repository.state.value.encounterCatalogErrorCode)
        assertEquals(null, repository.state.value.encounterDiscoveryErrorCode)
        assertEquals(null, repository.state.value.nearbyKitsuErrorCode)
        assertEquals(null, repository.state.value.funErrorCode)
        assertEquals(1, repository.state.value.encounterDiscovery.single {
            it.encounterCount > 0
        }.encounterCount)
        val initialCalls = transport.encounterDiscoveryCallCount

        transport.mockEncounterDiscoveryPage = discoveryPage("Turtle", 2, "mesh_message_rx")
        transport.emitRefresh(sequence = 7)
        withTimeout(5_000) {
            while (repository.state.value.encounterDiscovery.singleOrNull {
                    it.encounterCount > 0
                }?.encounterCount != 2
            ) delay(10)
        }
        assertTrue(transport.encounterDiscoveryCallCount > initialCalls)
        assertEquals("mesh_message_rx", repository.state.value.encounterDiscovery.single {
            it.encounterCount > 0
        }.lastSource)
        withTimeout(5_000) {
            while (cache.latestWrite?.encounterDiscovery?.singleOrNull {
                    it.encounterCount > 0
                }?.encounterCount != 2
            ) delay(10)
        }

        repository.disconnectByUser()
        assertFalse(repository.state.value.connection.connected)
        assertEquals(2, repository.state.value.encounterDiscovery.single {
            it.encounterCount > 0
        }.encounterCount)
        assertEquals("KT0001", cache.latestWrite?.encounterDiscoveryDeviceId)

        repository.connectAndRefresh(userInitiated = true)
        assertTrue(repository.state.value.connection.connected)
        assertEquals(null, repository.state.value.errorCode)
        assertEquals(2, repository.state.value.encounterDiscovery.single {
            it.encounterCount > 0
        }.encounterCount)
        scope.cancel()
    }

    @Test fun staleOwnerRefreshCannotCommitOrCacheAfterSelectingAnotherKitsu() = runBlocking {
        val profileA = FakeCredentials.bonded
        val profileB = profileA.copy(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            displayName = "Kitsu B",
            controllerIdB64 = "controller-b",
            controllerRootB64 = "root-b",
        )
        val credentials = SwitchingCredentials(listOf(profileA, profileB), profileA)
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(deviceId = "KTA001", firmwareVersion = "0.20.2-test")
            mockEncounterDiscoveryPage = discoveryPage("Turtle", 1, "mesh_advert_rx")
            mockMessagePage = page(message("1", 0), "1")
        }
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope, credentials)
        repository.connectAndRefresh(userInitiated = true)

        val refreshStarted = CompletableDeferred<Unit>()
        val allowRefresh = CompletableDeferred<Unit>()
        transport.mockStatus = transport.mockStatus.copy(deviceId = "KTA002")
        transport.mockEncounterDiscoveryPage = discoveryPage("Turtle", 7, "mesh_message_rx")
        transport.mockMessagePage = page(message("7", 1), "7")
        transport.beforeStatus = {
            refreshStarted.complete(Unit)
            allowRefresh.await()
        }
        val staleRefresh = async { repository.refresh() }
        withTimeout(5_000) { refreshStarted.await() }

        repository.selectDevice(profileB.deviceAddress)
        val writesAfterSelection = cache.writeCount
        allowRefresh.complete(Unit)
        withTimeout(5_000) { staleRefresh.await() }

        assertEquals(profileB.deviceAddress, repository.state.value.activeDeviceAddress)
        assertNull(repository.state.value.status)
        assertTrue(repository.state.value.encounterDiscovery.isEmpty())
        assertTrue(repository.state.value.messages.isEmpty())
        assertNull(cache.latestWrite)
        assertEquals(writesAfterSelection, cache.writeCount)
        scope.cancel()
    }

    @Test fun staleOwnerRefreshCannotRewriteCacheAfterTheControllerIsForgotten() = runBlocking {
        val profile = FakeCredentials.bonded
        val credentials = SwitchingCredentials(listOf(profile), profile)
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(deviceId = "KTA001", firmwareVersion = "0.20.2-test")
            mockEncounterDiscoveryPage = discoveryPage("Frog", 1, "mesh_advert_rx")
            mockMessagePage = page(message("1", 0), "1")
        }
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope, credentials)
        repository.connectAndRefresh(userInitiated = true)
        assertTrue(cache.latestWrite != null)

        val refreshStarted = CompletableDeferred<Unit>()
        val allowRefresh = CompletableDeferred<Unit>()
        transport.mockStatus = transport.mockStatus.copy(deviceId = "STALE-A")
        transport.mockEncounterDiscoveryPage = discoveryPage("Frog", 8, "mesh_message_rx")
        transport.beforeStatus = {
            refreshStarted.complete(Unit)
            allowRefresh.await()
        }
        val staleRefresh = async { repository.refresh() }
        withTimeout(5_000) { refreshStarted.await() }

        repository.forgetController(profile.deviceAddress)
        val writesAfterForget = cache.writeCount
        allowRefresh.complete(Unit)
        withTimeout(5_000) { staleRefresh.await() }

        assertNull(repository.state.value.activeDeviceAddress)
        assertNull(repository.state.value.status)
        assertTrue(repository.state.value.encounterDiscovery.isEmpty())
        assertTrue(repository.state.value.messages.isEmpty())
        assertNull(cache.latestWrite)
        assertEquals(writesAfterForget, cache.writeCount)
        scope.cancel()
    }

    @Test fun staleFunMutationCannotPublishIntoANewlySelectedOwner() = runBlocking {
        val profileA = FakeCredentials.bonded
        val profileB = profileA.copy(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            controllerIdB64 = "controller-b",
            controllerRootB64 = "root-b",
        )
        val credentials = SwitchingCredentials(listOf(profileA, profileB), profileA)
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.2-test")
            mockMessagePage = page(message("1", 0), "1")
            mockFunMutationResult = mockFunState.copy(
                party = mockFunState.party.copy(
                    reward = mockFunState.party.reward.copy(partyBond = 77),
                ),
            )
        }
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope, credentials)
        repository.connectAndRefresh(userInitiated = true)

        val mutationStarted = CompletableDeferred<Unit>()
        val allowMutation = CompletableDeferred<Unit>()
        transport.beforeFunMutation = {
            mutationStarted.complete(Unit)
            allowMutation.await()
        }
        val staleMutation = async { repository.startExpedition(ExpeditionDuration.SHORT) }
        withTimeout(5_000) { mutationStarted.await() }

        repository.selectDevice(profileB.deviceAddress)
        allowMutation.complete(Unit)
        withTimeout(5_000) { staleMutation.await() }

        assertEquals(profileB.deviceAddress, repository.state.value.activeDeviceAddress)
        assertNull(repository.state.value.funState)
        assertFalse(repository.state.value.funMutationInFlight)
        scope.cancel()
    }

    @Test fun staleNeighborReceiptCannotAdvanceTheNewOwnersRoster() = runBlocking {
        val profileA = FakeCredentials.bonded
        val profileB = profileA.copy(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            controllerIdB64 = "controller-b",
            controllerRootB64 = "root-b",
        )
        val credentials = SwitchingCredentials(listOf(profileA, profileB), profileA)
        val neighbor = NearbyKitsu(
            deviceId = "KT1234",
            sessionNonce = 99,
            packId = 0x5CAC86A3L,
            appearance = 0,
            evolutionStage = 0,
            bond = 50,
            mood = 1,
            emote = 1,
            rssi = -60.0,
            snr = 8.0,
            lastSeenAgeMs = 100,
            nextSequence = 10,
        )
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.2-test")
            mockMessagePage = page(message("1", 0), "1")
            mockNearbyKitsuPage = NearbyKitsuPage(
                schema = ENCOUNTER_NEIGHBORS_SCHEMA,
                items = listOf(neighbor),
                supportedActions = listOf(NeighborInteractionKind.PET),
            )
        }
        val cache = BlockingCache(null).apply { allowRead.countDown() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val repository = repository(transport, cache, scope, credentials)
        repository.connectAndRefresh(userInitiated = true)

        val mutationStarted = CompletableDeferred<Unit>()
        val allowMutation = CompletableDeferred<Unit>()
        transport.beforeNeighborInteraction = {
            mutationStarted.complete(Unit)
            allowMutation.await()
        }
        val command = NeighborInteractionCommand(
            actionId = "00000000-0000-0000-0000-000000000001",
            targetDeviceId = neighbor.deviceId,
            targetSessionNonce = neighbor.sessionNonce,
            sequence = neighbor.nextSequence,
            kind = NeighborInteractionKind.PET,
            expiresAtEpoch = 1_800_000_000,
        )
        val staleMutation = async { repository.interactWithNeighbor(command) }
        withTimeout(5_000) { mutationStarted.await() }

        repository.connectDevice(profileB.deviceAddress)
        assertEquals(10L, repository.state.value.nearbyKitsu.single().nextSequence)
        allowMutation.complete(Unit)
        withTimeout(5_000) { staleMutation.await() }

        assertEquals(profileB.deviceAddress, repository.state.value.activeDeviceAddress)
        assertEquals(10L, repository.state.value.nearbyKitsu.single().nextSequence)
        scope.cancel()
    }

    private fun repository(
        transport: MockKitsuTransport,
        cache: OwnerCache,
        scope: CoroutineScope,
        credentials: CredentialStore = FakeCredentials,
    ) = OwnerRepository(
        coordinator = ConnectionCoordinator(transport),
        cache = cache,
        credentials = credentials,
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

    private fun discoveryPage(
        creatureName: String,
        count: Int,
        source: String,
    ) = EncounterDiscoveryPage(
        schema = "kitsu.encounter-discovery.v1",
        items = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            if (creature.name == creatureName) {
                EncounterDiscoveryRecord(creature.packId, count, source)
            } else {
                EncounterDiscoveryRecord(creature.packId, 0, null)
            }
        },
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
        @Volatile var writeCount: Int = 0

        override fun read(): CacheSnapshot? {
            readStarted.countDown()
            check(allowRead.await(5, TimeUnit.SECONDS)) { "cache_read_timeout" }
            return initialSnapshot
        }

        override fun write(snapshot: CacheSnapshot) {
            latestWrite = snapshot
            writeCount += 1
        }

        override fun clear() {
            latestWrite = null
        }
    }

    private object FakeCredentials : CredentialStore {
        val bonded = BondedCompanion(
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

    private class SwitchingCredentials(
        devices: List<BondedCompanion>,
        active: BondedCompanion,
    ) : CredentialStore {
        private val saved = devices.toMutableList()
        private var selected: BondedCompanion? = active

        override suspend fun bondedCompanion() = selected
        override suspend fun bondedCompanions() = saved.toList()
        override suspend fun saveBondedCompanion(value: BondedCompanion?) {
            selected = value
            if (value != null) {
                saved.removeAll { it.deviceAddress.equals(value.deviceAddress, ignoreCase = true) }
                saved += value
            }
        }
        override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? =
            saved.firstOrNull { it.deviceAddress.equals(deviceAddress, ignoreCase = true) }
                ?.also { selected = it }
        override suspend fun removeBondedCompanion(deviceAddress: String): Boolean {
            val removed = saved.removeAll {
                it.deviceAddress.equals(deviceAddress, ignoreCase = true)
            }
            if (selected?.deviceAddress.equals(deviceAddress, ignoreCase = true)) {
                selected = saved.firstOrNull()
            }
            return removed
        }
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
