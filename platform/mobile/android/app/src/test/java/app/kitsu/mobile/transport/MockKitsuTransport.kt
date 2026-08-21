package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.NeedLevels
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow

/** Unit-test transport. This file is never compiled into an application variant. */
class MockKitsuTransport(
    override val mode: ConnectionMode,
    var connectResult: ConnectResult = ConnectResult.Connected,
) : KitsuTransport {
    private val eventBus = MutableSharedFlow<EventEnvelope>(extraBufferCapacity = 16)
    val actions = mutableListOf<ActionCommand>()
    var connectCount = 0
    var disconnectCount = 0
    var connectDelayMillis = 0L
    var wifiProvisioningCount = 0
    var meshConfigurationCount = 0
    var gatewayConfigurationCount = 0
    var enrollmentBeginCount = 0
    var enrollmentFinishCount = 0
    var mockChannels = listOf(MeshChannel(0, true, "Public"))

    var mockStatus = KitsuStatus(
        deviceId = "KTDEAD",
        displayName = "Kitsu KTDEAD",
        companionName = "Fox",
        mood = "CONTENT",
        batteryPercent = 87,
        needs = NeedLevels(94, 76, 82),
        cursor = "mock:2",
        updatedAt = 1_775_638_400,
    )
    var mockHistory = listOf(
        HistoryEntry("h1", "mock:1", "advert", "Heard Alice", 1_775_638_300),
    )
    var mockPeers = listOf(Peer("A1B2C3D4E5F6", "Alice", lastHeardAt = 1_775_638_300))
    var mockMessages = listOf(
        Message(
            "m1",
            "mock:2",
            "inbound",
            "A1B2C3D4E5F6",
            text = "Hello",
            state = "received",
            occurredAt = 1_775_638_350,
        ),
    )

    override suspend fun connect(): ConnectResult {
        connectCount++
        if (connectDelayMillis > 0) delay(connectDelayMillis)
        return connectResult
    }

    override suspend fun disconnect() {
        disconnectCount++
    }

    override suspend fun status(): KitsuStatus = mockStatus
    override suspend fun history(after: String?, limit: Int): HistoryPage =
        HistoryPage(mockHistory.takeLast(boundedLimit(limit)), mockHistory.lastOrNull()?.cursor)
    override suspend fun peers(): PeerPage = PeerPage(mockPeers)
    override suspend fun messages(after: String?, limit: Int): MessagePage =
        MessagePage(mockMessages.takeLast(boundedLimit(limit)), mockMessages.lastOrNull()?.cursor)
    override suspend fun channels(): List<MeshChannel> = mockChannels

    override suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt {
        if (mode != ConnectionMode.DIRECT_BLE) throw TransportException("direct_ble_required")
        meshConfigurationCount++
        mockStatus = mockStatus.copy(mesh = mockStatus.mesh.copy(enabled = enabled, rxReady = enabled))
        return MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
    }

    override suspend fun action(command: ActionCommand): ActionReceipt {
        command.requireAllowed()
        actions += command
        return ActionReceipt(command.clientRequestId, true, "accepted")
    }

    override fun events(after: String?): Flow<EventEnvelope> = eventBus

    override suspend fun provisionWifi(credentials: WifiProvisioning): ProvisioningReceipt {
        if (mode != ConnectionMode.DIRECT_BLE) throw TransportException("direct_ble_required")
        wifiProvisioningCount++
        mockStatus = mockStatus.copy(
            lan = mockStatus.lan.copy(wifiConfigured = true, wifiState = "connected"),
        )
        return ProvisioningReceipt(true, "stored")
    }

    override suspend fun configureGateway(
        configuration: GatewayConfiguration,
    ): GatewayConfigurationReceipt {
        if (mode != ConnectionMode.DIRECT_BLE) throw TransportException("direct_ble_required")
        gatewayConfigurationCount++
        return GatewayConfigurationReceipt(true, "stored")
    }

    override suspend fun beginGatewayEnrollment(
        request: GatewayEnrollmentBeginBody,
    ): GatewayEnrollmentReceipt {
        if (mode != ConnectionMode.DIRECT_BLE) throw TransportException("direct_ble_required")
        enrollmentBeginCount++
        return GatewayEnrollmentReceipt(
            "kitsu.gateway-enrollment.receipt.v1",
            true,
            "physical_confirmation_required",
            request.enrollmentId,
            60_000,
        )
    }

    override suspend fun finishGatewayEnrollment(
        request: GatewayEnrollmentFinishBody,
    ): GatewayEnrollmentReceipt {
        if (mode != ConnectionMode.DIRECT_BLE) throw TransportException("direct_ble_required")
        enrollmentFinishCount++
        return GatewayEnrollmentReceipt(
            "kitsu.gateway-enrollment.receipt.v1",
            true,
            "ready_for_wifi",
            request.enrollmentId,
            300_000,
        )
    }

    fun emit(event: EventEnvelope) {
        eventBus.tryEmit(event)
    }
}
