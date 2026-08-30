package ptl.kitsu.app.repository

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
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
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.ControllerRole
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.MockKitsuTransport

class OwnerRepositoryCaretakerTest {
    @Test fun hydratedControllerRoleIsTheAttestedCaretakerRole() = runBlocking {
        withCaretakerRepository(MockKitsuTransport()) { repository ->
            assertEquals(ControllerRole.CARETAKER, repository.activeControllerRoleAfterHydration())
        }
    }

    @Test fun caretakerPairingUsesTheExplicitCaretakerServicePath() = runBlocking {
        val pairing = RecordingPairing()

        withCaretakerRepository(MockKitsuTransport(), pairingService = pairing) { repository ->
            val failure = runCatching {
                repository.pairCaretakerController("Care phone")
            }.exceptionOrNull()

            assertEquals("caretaker_pairing_probe", (failure as PairingException).code)
            assertEquals(0, pairing.ownerCalls)
            assertEquals(1, pairing.caretakerCalls)
            assertEquals("Care phone", pairing.caretakerLabel)
            assertEquals("caretaker_pairing_probe", repository.state.value.errorCode)
        }
    }

    @Test fun caretakerRefreshCallsOnlySharedPetEndpointsAndClearsOwnerData() = runBlocking {
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.4")
        }

        withCaretakerRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val state = repository.state.value

            assertTrue(state.history.isNotEmpty())
            assertTrue(state.companionProfileSupported)
            assertTrue(state.focusSupported)
            assertTrue(state.walkSupported)
            assertEquals(1, transport.companionProfileCallCount)
            assertEquals(1, transport.focusStateCallCount)
            assertEquals(1, transport.walkStateCallCount)

            assertEquals(0, transport.messagesCallCount)
            assertEquals(0, transport.encounterDiscoveryCallCount)
            assertTrue(state.peers.isEmpty())
            assertTrue(state.channels.isEmpty())
            assertTrue(state.messages.isEmpty())
            assertNull(state.messageJournalSession)
            assertFalse(state.messageMarkReadSupported)
            assertTrue(state.encounterCatalog.isEmpty())
            assertFalse(state.encounterCatalogSupported)
            assertTrue(state.encounterDiscovery.isEmpty())
            assertFalse(state.encounterDiscoverySupported)
            assertTrue(state.nearbyKitsu.isEmpty())
            assertFalse(state.nearbyKitsuSupported)
            assertNull(state.funState)
            assertFalse(state.funSupported)
        }
    }

    @Test fun caretakerEventRefreshNeverReadsMessages() = runBlocking {
        var statusCalls = 0
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.4")
            beforeStatus = { statusCalls += 1 }
        }

        withCaretakerRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            assertEquals(1, statusCalls)

            transport.emitRefresh()
            withTimeout(5_000) {
                while (statusCalls < 2) delay(10)
            }

            assertEquals(0, transport.messagesCallCount)
            assertTrue(repository.state.value.messages.isEmpty())
            assertNull(repository.state.value.messagesErrorCode)
        }
    }

    @Test fun caretakerColdHydrationNeverRestoresOwnerOnlyCachedData() = runBlocking {
        val transport = MockKitsuTransport()
        val cache = StaticCache(
            CacheSnapshot(
                status = transport.mockStatus,
                history = transport.mockHistory,
                peers = transport.mockPeers,
                channels = listOf(MeshChannel(0, true, "Public")),
                messages = transport.mockMessages,
                writtenAt = 1,
                deviceAddress = CARETAKER.deviceAddress,
                encounterDiscoveryDeviceId = transport.mockStatus.deviceId,
                encounterDiscovery = transport.mockEncounterDiscoveryPage.items,
            ),
        )

        withCaretakerRepository(transport, cache) { repository ->
            withTimeout(5_000) {
                while (repository.state.value.activeDeviceAddress == null) delay(10)
            }
            val state = repository.state.value

            assertEquals(transport.mockStatus, state.status)
            assertEquals(transport.mockHistory, state.history)
            assertTrue(state.peers.isEmpty())
            assertTrue(state.channels.isEmpty())
            assertTrue(state.messages.isEmpty())
            assertTrue(state.encounterDiscovery.isEmpty())
            assertFalse(state.encounterDiscoverySupported)
        }
    }

    private suspend fun withCaretakerRepository(
        transport: MockKitsuTransport,
        cache: OwnerCache = StaticCache(null),
        pairingService: ControllerPairingService = UnusedPairing,
        block: suspend (OwnerRepository) -> Unit,
    ) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            block(
                OwnerRepository(
                    coordinator = ConnectionCoordinator(transport),
                    cache = cache,
                    credentials = CaretakerCredentials,
                    pairingService = pairingService,
                    scope = scope,
                ),
            )
        } finally {
            scope.cancel()
        }
    }

    private class StaticCache(private var snapshot: CacheSnapshot?) : OwnerCache {
        override fun read(): CacheSnapshot? = snapshot
        override fun write(snapshot: CacheSnapshot) {
            this.snapshot = snapshot
        }
        override fun clear() {
            snapshot = null
        }
    }

    private object CaretakerCredentials : CredentialStore {
        override suspend fun bondedCompanion() = CARETAKER
        override suspend fun bondedCompanions() = listOf(CARETAKER)
        override suspend fun saveBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun selectBondedCompanion(deviceAddress: String) = CARETAKER
        override suspend fun removeBondedCompanion(deviceAddress: String) = false
        override suspend fun pendingBondedCompanion(): BondedCompanion? = null
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun pendingControllerForgetAddress(): String? = null
        override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) = Unit
    }

    private object UnusedPairing : ControllerPairingService {
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

    private class RecordingPairing : ControllerPairingService {
        var ownerCalls = 0
        var caretakerCalls = 0
        var caretakerLabel: String? = null

        override suspend fun pairController(
            label: String,
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion {
            ownerCalls += 1
            throw PairingException("owner_pairing_path_used")
        }

        override suspend fun pairCaretakerController(
            label: String,
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion {
            caretakerCalls += 1
            caretakerLabel = label
            throw PairingException("caretaker_pairing_probe")
        }

        override suspend fun finishPendingPairing(
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun repairBluetoothPairing(
            deviceAddress: String,
            onProgress: (BluetoothPairingRepairProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override fun cancelPairing() = Unit
    }

    private companion object {
        val CARETAKER = BondedCompanion(
            deviceAddress = "00:11:22:33:44:55",
            displayName = "Kitsu",
            controllerIdB64 = "caretaker-controller",
            controllerRootB64 = "caretaker-root",
            role = ControllerRole.CARETAKER,
        )
    }
}
