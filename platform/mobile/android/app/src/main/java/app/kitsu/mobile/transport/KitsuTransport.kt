package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionPolicy
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiRetryReceipt
import kotlinx.coroutines.flow.Flow

enum class ConnectionMode {
    CONNECTING,
    DIRECT_BLE,
    REMOTE_BACKEND,
    PERMISSION_REQUIRED,
    OFFLINE,
}

sealed interface ConnectResult {
    data object Connected : ConnectResult
    data object CompanionAbsent : ConnectResult
    data class PermissionRequired(val permissions: List<String>) : ConnectResult
    data class Failed(val code: String) : ConnectResult
}

class TransportException(val code: String, cause: Throwable? = null) :
    Exception(code, cause)

interface KitsuTransport {
    val mode: ConnectionMode

    fun isConnectedTo(deviceAddress: String): Boolean = false
    suspend fun connect(): ConnectResult
    suspend fun disconnect()
    suspend fun status(): KitsuStatus
    suspend fun history(after: String? = null, limit: Int = 50): HistoryPage
    suspend fun peers(): PeerPage
    suspend fun messages(after: String? = null, limit: Int = 50): MessagePage
    suspend fun action(command: ActionCommand): ActionReceipt
    fun events(after: String? = null): Flow<EventEnvelope>

    suspend fun channels(): List<MeshChannel> = emptyList()

    suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt =
        throw TransportException("direct_ble_required")

    suspend fun provisionWifi(credentials: WifiProvisioning): ProvisioningReceipt =
        throw TransportException("direct_ble_required")

    suspend fun retryWifi(): WifiRetryReceipt =
        throw TransportException("direct_ble_required")

    suspend fun configureGateway(configuration: GatewayConfiguration): GatewayConfigurationReceipt =
        throw TransportException("direct_ble_required")

    suspend fun beginGatewayEnrollment(request: GatewayEnrollmentBeginBody): GatewayEnrollmentReceipt =
        throw TransportException("direct_ble_required")

    suspend fun finishGatewayEnrollment(request: GatewayEnrollmentFinishBody): GatewayEnrollmentReceipt =
        throw TransportException("direct_ble_required")
}

fun ActionCommand.requireAllowed() {
    ActionPolicy.validate(this)?.let { throw TransportException(it) }
}

fun boundedLimit(limit: Int): Int = limit.coerceIn(1, 100)
