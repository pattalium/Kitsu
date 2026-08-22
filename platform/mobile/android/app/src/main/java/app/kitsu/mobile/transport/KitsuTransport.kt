package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionPolicy
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.ControllerForgetReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.update.FirmwareUpdateReceipt
import kotlinx.coroutines.flow.Flow

enum class ConnectionMode {
    CONNECTING,
    DIRECT_BLE,
    PERMISSION_REQUIRED,
    OFFLINE,
}

sealed interface ConnectResult {
    data object Connected : ConnectResult
    data object CompanionAbsent : ConnectResult
    data class PermissionRequired(val permissions: List<String>) : ConnectResult
    data class Failed(val code: String) : ConnectResult
}

class TransportException(val code: String, cause: Throwable? = null) : Exception(code, cause)

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
    suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt
    suspend fun forgetController(): ControllerForgetReceipt

    fun firmwareTransferChunkBytes(): Int = 4_096
    suspend fun firmwareUpdateStatus(): FirmwareUpdateReceipt
    suspend fun beginFirmwareUpdate(manifest: ByteArray, signature: ByteArray): FirmwareUpdateReceipt
    suspend fun writeFirmwareUpdate(updateId: String, offset: Int, data: ByteArray): FirmwareUpdateReceipt
    suspend fun finishFirmwareUpdate(updateId: String): FirmwareUpdateReceipt
    suspend fun rebootFirmwareUpdate(updateId: String): FirmwareUpdateReceipt
    suspend fun abortFirmwareUpdate(updateId: String): FirmwareUpdateReceipt
}

fun ActionCommand.requireAllowed() {
    ActionPolicy.validate(this)?.let { throw TransportException(it) }
}

fun boundedLimit(limit: Int): Int = limit.coerceIn(1, 100)
