package ptl.kitsu.app.repository

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionBond
import ptl.kitsu.app.model.CompanionCheckIn
import ptl.kitsu.app.model.CompanionComfort
import ptl.kitsu.app.model.CompanionComfortKind
import ptl.kitsu.app.model.CompanionDailyGoal
import ptl.kitsu.app.model.CompanionDevelopment
import ptl.kitsu.app.model.CompanionGoalKind
import ptl.kitsu.app.model.CompanionMood
import ptl.kitsu.app.model.CompanionPersonalBests
import ptl.kitsu.app.model.CompanionPersonality
import ptl.kitsu.app.model.CompanionPreferences
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.CompanionQuickAction
import ptl.kitsu.app.model.CompanionQuietHours
import ptl.kitsu.app.model.CompanionRequest
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.CompanionSettings
import ptl.kitsu.app.model.FocusCompletion
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusPrompt
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.FocusStartCommand
import ptl.kitsu.app.model.PET_FEATURE_SCHEMA_VERSION
import ptl.kitsu.app.model.PetPersonality
import ptl.kitsu.app.model.PetPresentationAnimation
import ptl.kitsu.app.model.PetPresentationFrame
import ptl.kitsu.app.model.PetPresentationPack
import ptl.kitsu.app.model.PetPresentationPlayback
import ptl.kitsu.app.model.PetPresentationRole
import ptl.kitsu.app.model.PetPresentationState
import ptl.kitsu.app.model.PetPresentationSurface
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkOutcome
import ptl.kitsu.app.model.WalkPhase
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkStartCommand
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.MockKitsuTransport
import ptl.kitsu.app.transport.TransportException
import java.security.MessageDigest

class OwnerRepositoryPetFeatureTest {
    @Test
    fun oldFirmwareIsNeverProbedAndMutationsFailLocally() = runBlocking {
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.3")
        }
        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)

            assertFalse(repository.state.value.companionProfileSupported)
            assertFalse(repository.state.value.focusSupported)
            assertFalse(repository.state.value.walkSupported)
            assertEquals(0, transport.companionProfileCallCount)
            assertEquals(0, transport.focusStateCallCount)
            assertEquals(0, transport.walkStateCallCount)

            val failure = runCatching {
                repository.startFocus(FocusStartCommand(1L, 25))
            }.exceptionOrNull() as TransportException
            assertEquals("firmware_operation_unavailable", failure.code)
            assertEquals(0, transport.focusMutationCount)
        }
    }

    @Test
    fun supportedRefreshAndMutationsPublishOnlyReturnedFirmwareState() = runBlocking {
        val initialProfile = profile("KITSU")
        val renamedProfile = profile("NOVA")
        val initialFocus = idleFocus()
        val runningFocus = runningFocus(41L)
        val initialWalk = idleWalk()
        val activeWalk = activeWalk(91L)
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.4-test")
            mockCompanionProfile = initialProfile
            mockCompanionProfileMutationResult = renamedProfile
            mockFocusState = initialFocus
            mockFocusMutationResult = runningFocus
            mockWalkState = initialWalk
            mockWalkMutationResult = activeWalk
        }

        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val refreshed = repository.state.value
            assertTrue(refreshed.companionProfileSupported)
            assertTrue(refreshed.focusSupported)
            assertTrue(refreshed.walkSupported)
            assertSame(initialProfile, refreshed.companionProfile)
            assertSame(initialFocus, refreshed.focusState)
            assertSame(initialWalk, refreshed.walkState)
            assertEquals(1, transport.companionProfileCallCount)
            assertEquals(1, transport.focusStateCallCount)
            assertEquals(1, transport.walkStateCallCount)

            assertSame(renamedProfile, repository.setCompanionNickname("NOVA"))
            assertSame(runningFocus, repository.startFocus(FocusStartCommand(41L, 25)))
            assertSame(
                activeWalk,
                repository.startWalk(
                    WalkStartCommand(
                        WalkTerrain.MEADOW,
                        WalkObjective.EXPLORE,
                        WalkRisk.BALANCED,
                        WalkWeather.CLEAR,
                        1_000L,
                        false,
                    ),
                ),
            )

            val mutated = repository.state.value
            assertSame(renamedProfile, mutated.companionProfile)
            assertSame(runningFocus, mutated.focusState)
            assertSame(activeWalk, mutated.walkState)
            assertNull(mutated.companionProfileErrorCode)
            assertNull(mutated.focusErrorCode)
            assertNull(mutated.walkErrorCode)
            assertFalse(mutated.companionProfileMutationInFlight)
            assertFalse(mutated.focusMutationInFlight)
            assertFalse(mutated.walkMutationInFlight)
        }
    }

    @Test
    fun mutationFailureIsVisibleAndAllPetMutationsAreSerialized() = runBlocking {
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.4")
            mockCompanionProfile = profile("KITSU")
            mockFocusState = idleFocus()
            mockFocusMutationResult = runningFocus(5L)
            mockWalkState = idleWalk()
            mockWalkMutationResult = activeWalk(7L)
        }
        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)

            transport.beforePetMutation = { throw TransportException("wrong_phase") }
            val failure = runCatching { repository.stopFocus(5L) }.exceptionOrNull()
                as TransportException
            assertEquals("wrong_phase", failure.code)
            assertEquals("wrong_phase", repository.state.value.focusErrorCode)
            assertFalse(repository.state.value.focusMutationInFlight)

            val firstStarted = CompletableDeferred<Unit>()
            val releaseFirst = CompletableDeferred<Unit>()
            var invocation = 0
            transport.beforePetMutation = {
                invocation += 1
                if (invocation == 1) {
                    firstStarted.complete(Unit)
                    releaseFirst.await()
                }
            }
            val walk = async {
                repository.startWalk(
                    WalkStartCommand(
                        WalkTerrain.MEADOW,
                        WalkObjective.EXPLORE,
                        WalkRisk.BALANCED,
                        WalkWeather.CLEAR,
                        1_000L,
                        false,
                    ),
                )
            }
            withTimeout(5_000) { firstStarted.await() }
            val focusCallsBefore = transport.focusMutationCount
            val focus = async { repository.startFocus(FocusStartCommand(5L, 25)) }
            delay(100)
            assertEquals(focusCallsBefore, transport.focusMutationCount)
            assertTrue(repository.state.value.walkMutationInFlight)

            releaseFirst.complete(Unit)
            withTimeout(5_000) {
                walk.await()
                focus.await()
            }
            assertEquals(focusCallsBefore + 1, transport.focusMutationCount)
            assertFalse(repository.state.value.walkMutationInFlight)
            assertFalse(repository.state.value.focusMutationInFlight)
            assertNull(repository.state.value.focusErrorCode)
        }
    }

    @Test
    fun staleMutationCannotPublishIntoAnotherSelectedOwner() = runBlocking {
        val profileA = TestCredentials.DEFAULT
        val profileB = profileA.copy(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            displayName = "Kitsu B",
            controllerIdB64 = "controller-b",
            controllerRootB64 = "root-b",
        )
        val credentials = SwitchingCredentials(listOf(profileA, profileB), profileA)
        val staleProfile = profile("STALE")
        val transport = MockKitsuTransport().apply {
            mockStatus = mockStatus.copy(firmwareVersion = "0.20.4")
            mockCompanionProfile = profile("KITSU")
            mockCompanionProfileMutationResult = staleProfile
            mockFocusState = idleFocus()
            mockWalkState = idleWalk()
        }
        val mutationStarted = CompletableDeferred<Unit>()
        val releaseMutation = CompletableDeferred<Unit>()
        transport.beforePetMutation = {
            mutationStarted.complete(Unit)
            releaseMutation.await()
        }

        withRepository(transport, credentials) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val mutation = async { repository.setCompanionNickname("STALE") }
            withTimeout(5_000) { mutationStarted.await() }

            repository.selectDevice(profileB.deviceAddress)
            releaseMutation.complete(Unit)
            withTimeout(5_000) { mutation.await() }

            val selected = repository.state.value
            assertEquals(profileB.deviceAddress, selected.activeDeviceAddress)
            assertNull(selected.companionProfile)
            assertFalse(selected.companionProfileSupported)
            assertFalse(selected.companionProfileMutationInFlight)
        }
    }

    @Test
    fun presentationCaptureReadsExactChunksVerifiesDigestAndAlwaysCloses() = runBlocking {
        val frame = ByteArray(512) { index -> (index * 31).toByte() }
        val transport = supportedTransport().apply {
            mockPetPresentationState = presentationState(frame)
            mockPetPresentationFrame = frame
        }

        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val snapshot = repository.capturePetPresentation()

            assertArrayEquals(frame, snapshot.frame)
            assertArrayEquals(frame, repository.state.value.petPresentationFrame)
            assertEquals(1, transport.petPresentationOpenCount)
            assertEquals(3, transport.petPresentationReadCount)
            assertEquals(1, transport.petPresentationCloseCount)
            assertFalse(repository.state.value.petPresentationInFlight)
            assertNull(repository.state.value.petPresentationErrorCode)
        }
    }

    @Test
    fun presentationDigestMismatchPublishesNoFrameAndStillCloses() = runBlocking {
        val frame = ByteArray(512) { 0x55.toByte() }
        val transport = supportedTransport().apply {
            mockPetPresentationState = presentationState(frame).copy(
                frame = presentationState(frame).frame.copy(sha256 = "0".repeat(64)),
            )
            mockPetPresentationFrame = frame
        }

        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val failure = runCatching { repository.capturePetPresentation() }.exceptionOrNull()
                as TransportException

            assertEquals("pet_presentation_digest_mismatch", failure.code)
            assertEquals(1, transport.petPresentationCloseCount)
            assertNull(repository.state.value.petPresentation)
            assertNull(repository.state.value.petPresentationFrame)
            assertEquals(
                "pet_presentation_digest_mismatch",
                repository.state.value.petPresentationErrorCode,
            )
            assertFalse(repository.state.value.petPresentationInFlight)
        }
    }

    @Test
    fun repeatedPresentationCaptureFailsImmediatelyInsteadOfQueueingAnotherSession() = runBlocking {
        val frame = ByteArray(512) { it.toByte() }
        val openStarted = CompletableDeferred<Unit>()
        val releaseOpen = CompletableDeferred<Unit>()
        val transport = supportedTransport().apply {
            mockPetPresentationState = presentationState(frame)
            mockPetPresentationFrame = frame
            beforePetPresentationOpen = {
                openStarted.complete(Unit)
                releaseOpen.await()
            }
        }

        withRepository(transport) { repository ->
            repository.connectAndRefresh(userInitiated = true)
            val first = async { repository.capturePetPresentation() }
            withTimeout(5_000) { openStarted.await() }

            val second = runCatching { repository.capturePetPresentation() }.exceptionOrNull()
                as TransportException
            assertEquals("pet_presentation_in_flight", second.code)
            assertEquals(1, transport.petPresentationOpenCount)

            releaseOpen.complete(Unit)
            withTimeout(5_000) { first.await() }
            assertEquals(1, transport.petPresentationOpenCount)
        }
    }

    private fun supportedTransport() = MockKitsuTransport().apply {
        mockStatus = mockStatus.copy(firmwareVersion = "0.20.4")
        mockCompanionProfile = profile("KITSU")
        mockFocusState = idleFocus()
        mockWalkState = idleWalk()
    }

    private fun presentationState(frameBytes: ByteArray) = PetPresentationState(
        ok = true,
        schema = 1,
        sessionId = 1,
        capturedAtMs = 1,
        surface = PetPresentationSurface.PET,
        displayAwake = true,
        frameVisible = true,
        pack = PetPresentationPack(
            valid = true,
            name = "FOX GIRL",
            id = 7,
            revision = 3,
            totalBytes = 64L + 48L * frameBytes.size,
            payloadCrc32 = 1,
            headerCrc32 = 2,
            format = 1,
            width = 64,
            height = 64,
            frameCount = 48,
            appearance = 0,
        ),
        animation = PetPresentationAnimation(
            active = true,
            finite = false,
            requestedRole = PetPresentationRole.IDLE,
            resolvedRole = PetPresentationRole.IDLE,
            playback = PetPresentationPlayback.LOOP,
            token = 1,
            elapsedMs = 0,
        ),
        frame = PetPresentationFrame(
            available = true,
            encoding = "xbm_row_major_lsb_first",
            bytes = frameBytes.size,
            sha256 = MessageDigest.getInstance("SHA-256").digest(frameBytes)
                .joinToString("") { "%02X".format(it) },
        ),
    )

    private suspend fun withRepository(
        transport: MockKitsuTransport,
        credentials: CredentialStore = TestCredentials,
        block: suspend (OwnerRepository) -> Unit,
    ) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            block(
                OwnerRepository(
                    coordinator = ConnectionCoordinator(transport),
                    cache = MemoryCache,
                    credentials = credentials,
                    pairingService = TestPairing,
                    scope = scope,
                ),
            )
        } finally {
            scope.cancel()
        }
    }

    private fun profile(nickname: String) = CompanionProfile(
        ok = true,
        schema = PET_FEATURE_SCHEMA_VERSION,
        nickname = nickname,
        personality = CompanionPersonality(PetPersonality.GENTLE, 70, 60, 50, 80),
        mood = CompanionMood.CONTENT,
        bond = CompanionBond(0, 0, 0, 0),
        favorite = null,
        routine = null,
        ritual = null,
        preferences = CompanionPreferences(null, null, null),
        checkIn = CompanionCheckIn(
            request = CompanionRequest(CompanionRequestState.NONE, CompanionAction.PET),
            question = null,
            comfort = CompanionComfort(
                CompanionComfortKind.NONE,
                "I'M ALL RIGHT",
                "RIGHT HERE",
            ),
            callbackReady = false,
        ),
        goal = CompanionDailyGoal(CompanionGoalKind.CARE, CompanionAction.PET, 0, 2),
        development = CompanionDevelopment(
            0,
            0,
            0,
            0,
            CompanionPersonalBests(0, 0, 0),
        ),
        settings = CompanionSettings(
            CompanionQuickAction.PET,
            CompanionQuietHours(false, 0, 0),
        ),
        latestMemory = null,
    )

    private fun idleFocus() = FocusSessionState(
        true,
        PET_FEATURE_SCHEMA_VERSION,
        FocusPhase.IDLE,
        FocusCompletion.NONE,
        0,
        0,
        0,
        0,
        0,
        0,
        FocusPrompt("", "", false),
    )

    private fun runningFocus(sessionId: Long) = FocusSessionState(
        true,
        PET_FEATURE_SCHEMA_VERSION,
        FocusPhase.FOCUS,
        FocusCompletion.NONE,
        sessionId,
        25,
        5,
        0,
        1_800_000,
        1,
        FocusPrompt("FOCUS TIME", "STAY WITH IT", false),
    )

    private fun idleWalk() = WalkAdventureState(
        true,
        PET_FEATURE_SCHEMA_VERSION,
        WalkPhase.IDLE,
        WalkOutcome.NONE,
        0,
        0,
        0,
        0,
        0,
        WalkTerrain.MEADOW,
        WalkObjective.EXPLORE,
        WalkRisk.BALANCED,
        WalkWeather.UNKNOWN,
        PetPersonality.GENTLE,
        0,
        0,
        WalkPrivacy.OFF,
        0,
        0,
        0,
        0,
        0,
        null,
    )

    private fun activeWalk(routeId: Long) = idleWalk().copy(
        phase = WalkPhase.ACTIVE,
        routeId = routeId,
        targetSteps = 1_000,
        weather = WalkWeather.CLEAR,
    )

    private object MemoryCache : OwnerCache {
        override fun read(): CacheSnapshot? = null
        override fun write(snapshot: CacheSnapshot) = Unit
        override fun clear() = Unit
    }

    private object TestCredentials : CredentialStore {
        val DEFAULT = BondedCompanion(
            deviceAddress = "00:11:22:33:44:55",
            displayName = "Kitsu",
            controllerIdB64 = "controller",
            controllerRootB64 = "root",
        )

        override suspend fun bondedCompanion() = DEFAULT
        override suspend fun bondedCompanions() = listOf(DEFAULT)
        override suspend fun saveBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun selectBondedCompanion(deviceAddress: String) = DEFAULT
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
        }
        override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? =
            saved.firstOrNull { it.deviceAddress.equals(deviceAddress, ignoreCase = true) }
                ?.also { selected = it }
        override suspend fun removeBondedCompanion(deviceAddress: String) = saved.removeAll {
            it.deviceAddress.equals(deviceAddress, ignoreCase = true)
        }
        override suspend fun pendingBondedCompanion(): BondedCompanion? = null
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun pendingControllerForgetAddress(): String? = null
        override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) = Unit
    }

    private object TestPairing : ControllerPairingService {
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
