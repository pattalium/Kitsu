package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.ENCOUNTER_CATALOG_SCHEMA
import ptl.kitsu.app.model.ENCOUNTER_NEIGHBORS_SCHEMA
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.EncounterCatalogPage
import ptl.kitsu.app.model.EncounterDiscoveryPage
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.ExpeditionFunState
import ptl.kitsu.app.model.ExpeditionStatus
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.FocusStartCommand
import ptl.kitsu.app.model.FUN_STATE_SCHEMA
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionReceipt
import ptl.kitsu.app.model.NearbyKitsuPage
import ptl.kitsu.app.model.PartyFunState
import ptl.kitsu.app.model.PartyPhase
import ptl.kitsu.app.model.PartyReward
import ptl.kitsu.app.model.PartyRewardTier
import ptl.kitsu.app.model.PartyRole
import ptl.kitsu.app.model.PartySignalChoice
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.model.PetPresentationChunk
import ptl.kitsu.app.model.PetPresentationState
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
import ptl.kitsu.app.model.StoryFunState
import ptl.kitsu.app.model.StoryStatus
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkDecisionCommand
import ptl.kitsu.app.model.WalkLocationCommand
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkStartCommand
import ptl.kitsu.app.model.WalkSyncCommand
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.serialization.json.buildJsonObject

class MockKitsuTransport(
    override val mode: ConnectionMode = ConnectionMode.DIRECT_BLE,
    var connectResult: ConnectResult = ConnectResult.Connected,
) : KitsuTransport {
    private val eventBus = MutableSharedFlow<EventEnvelope>(extraBufferCapacity = 16)
    val actions = mutableListOf<ActionCommand>()
    var connectCount = 0
    var disconnectCount = 0
    var clockSyncCount = 0
    var connectDelayMillis = 0L
    var connectedAddress: String? = null
    var meshConfigurationCount = 0
    var messagesCallCount = 0
    var encounterDiscoveryCallCount = 0
    var companionProfileCallCount = 0
    var focusStateCallCount = 0
    var walkStateCallCount = 0
    var companionProfileMutationCount = 0
    var focusMutationCount = 0
    var walkMutationCount = 0
    var petPresentationOpenCount = 0
    var petPresentationReadCount = 0
    var petPresentationCloseCount = 0
    var mockChannels = listOf(MeshChannel(0, true, "Public"))
    var mockMessagePage: MessagePage? = null
    var beforeStatus: (suspend () -> Unit)? = null
    var beforeMessages: (suspend () -> Unit)? = null
    var beforeNeighborInteraction: (suspend () -> Unit)? = null
    var beforeFunMutation: (suspend () -> Unit)? = null
    var beforePetMutation: (suspend () -> Unit)? = null
    var beforePetPresentationOpen: (suspend () -> Unit)? = null
    var mockEncounterDiscoveryPage = EncounterDiscoveryPage(
        schema = "kitsu.encounter-discovery.v1",
        items = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            EncounterDiscoveryRecord(creature.packId, 0, null)
        },
    )

    override fun isConnectedTo(deviceAddress: String): Boolean =
        connectedAddress?.equals(deviceAddress, ignoreCase = true) == true

    var mockStatus = KitsuStatus(
        deviceId = "KTDEAD",
        companionName = "Fox",
        mood = "CONTENT",
        batteryPercent = 87,
        needs = NeedLevels(94, 76, 82),
        cursor = "mock:2",
        updatedAt = 1_775_638_400,
    )
    var mockHistory = listOf(HistoryEntry("h1", "mock:1", "advert", "Heard Alice", 1_775_638_300))
    var mockPeers = listOf(Peer("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "Alice"))
    var mockMessages = listOf(
        Message("m1", "mock:2", "inbound", mockPeers.single().id, text = "Hello", state = "received", occurredAt = 1),
    )

    override suspend fun connect(): ConnectResult {
        connectCount++
        if (connectDelayMillis > 0) delay(connectDelayMillis)
        return connectResult
    }

    override suspend fun disconnect() { disconnectCount++ }
    override suspend fun synchronizeClock() { clockSyncCount++ }
    override suspend fun status(): KitsuStatus {
        beforeStatus?.invoke()
        return mockStatus
    }
    override suspend fun history(after: String?, limit: Int) = HistoryPage(mockHistory.takeLast(boundedLimit(limit)))
    override suspend fun peers() = PeerPage(mockPeers)
    override suspend fun messages(after: String?, limit: Int): MessagePage {
        beforeMessages?.invoke()
        messagesCallCount += 1
        return mockMessagePage ?: MessagePage(mockMessages.takeLast(boundedLimit(limit)))
    }
    override suspend fun channels(firmwareVersion: String?): List<MeshChannel> = mockChannels
    override suspend fun encounterCatalog() = EncounterCatalogPage(
        ENCOUNTER_CATALOG_SCHEMA,
        PUBLIC_ENCOUNTER_CATALOG,
    )
    override suspend fun encounterDiscovery(): EncounterDiscoveryPage {
        encounterDiscoveryCallCount += 1
        return mockEncounterDiscoveryPage
    }
    var mockNearbyKitsuPage = NearbyKitsuPage(ENCOUNTER_NEIGHBORS_SCHEMA)
    var mockFunState = FunState(
        schema = FUN_STATE_SCHEMA,
        expedition = ExpeditionFunState(ExpeditionStatus.IDLE, null, null, 0, 0, 0, null),
        story = StoryFunState(StoryStatus.IDLE, null, null),
        party = PartyFunState(
            role = PartyRole.NONE,
            phase = PartyPhase.IDLE,
            hostDeviceId = null,
            sessionNonce = null,
            discoveredHosts = emptyList(),
            participantCount = 0,
            participants = emptyList(),
            round = 0,
            localChoice = PartySignalChoice.NONE,
            reward = PartyReward(PartyRewardTier.NONE, 0, 0, 0, 0, 0, 0, 0),
        ),
    )
    var mockFunMutationResult: FunState? = null
    var mockCompanionProfile: CompanionProfile? = null
    var mockCompanionProfileMutationResult: CompanionProfile? = null
    var mockFocusState: FocusSessionState? = null
    var mockFocusMutationResult: FocusSessionState? = null
    var mockWalkState: WalkAdventureState? = null
    var mockWalkMutationResult: WalkAdventureState? = null
    var mockPetPresentationState: PetPresentationState? = null
    var mockPetPresentationFrame: ByteArray? = null

    override suspend fun nearbyKitsu() = mockNearbyKitsuPage
    override suspend fun neighborInteraction(
        command: NeighborInteractionCommand,
    ): NeighborInteractionReceipt {
        beforeNeighborInteraction?.invoke()
        return NeighborInteractionReceipt(
            schema = "kitsu.neighbor-action.v1",
            actionId = command.actionId,
            accepted = true,
            state = "applied",
        )
    }
    override suspend fun funState() = mockFunState
    override suspend fun startExpedition(duration: ExpeditionDuration): FunState {
        val result = mockFunMutationResult ?: mockFunState
        beforeFunMutation?.invoke()
        return result
    }
    override suspend fun companionProfile(): CompanionProfile {
        companionProfileCallCount += 1
        return mockCompanionProfile ?: throw TransportException("mock_companion_profile_missing")
    }
    override suspend fun setCompanionNickname(nickname: String): CompanionProfile =
        mutateCompanionProfile()
    override suspend fun answerCompanionRequest(accept: Boolean): CompanionProfile =
        mutateCompanionProfile()
    override suspend fun answerCompanionQuestion(choice: Int): CompanionProfile =
        mutateCompanionProfile()
    override suspend fun focusState(): FocusSessionState {
        focusStateCallCount += 1
        return mockFocusState ?: throw TransportException("mock_focus_state_missing")
    }
    override suspend fun startFocus(command: FocusStartCommand): FocusSessionState = mutateFocus()
    override suspend fun stopFocus(sessionId: Long): FocusSessionState = mutateFocus()
    override suspend fun cancelFocus(sessionId: Long): FocusSessionState = mutateFocus()
    override suspend fun acknowledgeFocus(sessionId: Long): FocusSessionState = mutateFocus()
    override suspend fun walkState(): WalkAdventureState {
        walkStateCallCount += 1
        return mockWalkState ?: throw TransportException("mock_walk_state_missing")
    }
    override suspend fun startWalk(command: WalkStartCommand): WalkAdventureState = mutateWalk()
    override suspend fun syncWalk(command: WalkSyncCommand): WalkAdventureState = mutateWalk()
    override suspend fun updateWalkLocation(command: WalkLocationCommand): WalkAdventureState = mutateWalk()
    override suspend fun decideWalk(command: WalkDecisionCommand): WalkAdventureState = mutateWalk()
    override suspend fun finishWalk(routeId: Long): WalkAdventureState = mutateWalk()
    override suspend fun acknowledgeWalk(routeId: Long): WalkAdventureState = mutateWalk()
    override suspend fun setWalkPrivacy(mode: WalkPrivacy): WalkAdventureState = mutateWalk()
    override suspend fun setWalkHome(zoneToken: Long): WalkAdventureState = mutateWalk()
    override suspend fun openPetPresentation(sessionId: Long): PetPresentationState {
        petPresentationOpenCount += 1
        beforePetPresentationOpen?.invoke()
        return mockPetPresentationState?.copy(sessionId = sessionId)
            ?: throw TransportException("mock_pet_presentation_missing")
    }
    override suspend fun readPetPresentation(
        sessionId: Long,
        offset: Int,
        bytes: Int,
        expectedFrameBytes: Int,
        expectedFrameSha256: String,
    ): PetPresentationChunk {
        petPresentationReadCount += 1
        val frame = mockPetPresentationFrame
            ?: throw TransportException("mock_pet_presentation_frame_missing")
        if (frame.size != expectedFrameBytes || offset !in 0..frame.size ||
            bytes !in 1..PetPresentationWireCodec.MAX_CHUNK_BYTES || offset + bytes > frame.size
        ) throw TransportException("mock_pet_presentation_read_invalid")
        val nextOffset = offset + bytes
        return PetPresentationChunk(
            sessionId = sessionId,
            offset = offset,
            nextOffset = nextOffset,
            complete = nextOffset == frame.size,
            frameSha256 = expectedFrameSha256,
            data = frame.copyOfRange(offset, nextOffset),
        )
    }
    override suspend fun closePetPresentation(sessionId: Long): Boolean {
        petPresentationCloseCount += 1
        return true
    }
    override suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt {
        meshConfigurationCount++
        mockStatus = mockStatus.copy(mesh = mockStatus.mesh.copy(enabled = enabled))
        return MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
    }
    override suspend fun action(command: ActionCommand): ActionReceipt {
        command.requireAllowed()
        actions += command
        return ActionReceipt(
            command.clientRequestId,
            true,
            if (command.kind in setOf(ActionKind.SEND_MESSAGE, ActionKind.ADVERTISE_ONCE)) "queued" else "applied",
        )
    }
    override fun events(after: String?): Flow<EventEnvelope> = eventBus
    suspend fun emitRefresh(sequence: Long = 1L) {
        eventBus.emit(EventEnvelope(1, "ble:$sequence", "refresh", buildJsonObject {}))
    }
    override suspend fun forgetController() = ControllerForgetReceipt("kitsu.controller-forget.v1", true)

    override suspend fun firmwareUpdateStatus() = updateReceipt("idle", null, 0, 0)
    override suspend fun beginFirmwareUpdate(manifest: ByteArray, signature: ByteArray) =
        updateReceipt("receiving", "0".repeat(64), 1, 0)
    override suspend fun writeFirmwareUpdate(updateId: String, offset: Int, data: ByteArray) =
        updateReceipt("receiving", updateId, offset + data.size, offset + data.size)
    override suspend fun finishFirmwareUpdate(updateId: String) =
        updateReceipt("ready_to_reboot", updateId, 1, 1)
    override suspend fun rebootFirmwareUpdate(updateId: String) =
        updateReceipt("ready_to_reboot", updateId, 1, 1).copy(scheduled = true)
    override suspend fun abortFirmwareUpdate(updateId: String) = updateReceipt("idle", null, 0, 0)

    private suspend fun mutateCompanionProfile(): CompanionProfile {
        companionProfileMutationCount += 1
        beforePetMutation?.invoke()
        return mockCompanionProfileMutationResult ?: mockCompanionProfile
            ?: throw TransportException("mock_companion_profile_missing")
    }

    private suspend fun mutateFocus(): FocusSessionState {
        focusMutationCount += 1
        beforePetMutation?.invoke()
        return mockFocusMutationResult ?: mockFocusState
            ?: throw TransportException("mock_focus_state_missing")
    }

    private suspend fun mutateWalk(): WalkAdventureState {
        walkMutationCount += 1
        beforePetMutation?.invoke()
        return mockWalkMutationResult ?: mockWalkState
            ?: throw TransportException("mock_walk_state_missing")
    }

    private fun updateReceipt(state: String, id: String?, imageBytes: Int, nextOffset: Int) = FirmwareUpdateReceipt(
        ok = true,
        protocol = 1,
        state = state,
        updateId = id,
        firmwareVersion = "0.13.0-test",
        imageBytes = imageBytes,
        nextOffset = nextOffset,
        chunkBytes = 4_096,
        resumed = false,
        replayed = false,
        scheduled = false,
    )
}
