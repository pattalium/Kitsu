package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.PeerPage
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
    var mockChannels = listOf(MeshChannel(0, true, "Public"))
    var mockMessagePage: MessagePage? = null

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
    override suspend fun status(): KitsuStatus = mockStatus
    override suspend fun history(after: String?, limit: Int) = HistoryPage(mockHistory.takeLast(boundedLimit(limit)))
    override suspend fun peers() = PeerPage(mockPeers)
    override suspend fun messages(after: String?, limit: Int): MessagePage {
        messagesCallCount += 1
        return mockMessagePage ?: MessagePage(mockMessages.takeLast(boundedLimit(limit)))
    }
    override suspend fun channels(firmwareVersion: String?): List<MeshChannel> = mockChannels
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
