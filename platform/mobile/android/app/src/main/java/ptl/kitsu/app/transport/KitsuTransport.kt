package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionPolicy
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MessageMarkReadReceipt
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.update.FirmwareUpdateReceipt
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
    suspend fun synchronizeClock()
    suspend fun status(): KitsuStatus
    suspend fun history(after: String? = null, limit: Int = 50): HistoryPage
    suspend fun peers(): PeerPage
    suspend fun messages(after: String? = null, limit: Int = 50): MessagePage
    suspend fun markMessagesRead(
        journalSession: String,
        messageIds: List<String>,
    ): MessageMarkReadReceipt = throw TransportException("firmware_operation_unavailable")
    suspend fun action(command: ActionCommand): ActionReceipt
    fun events(after: String? = null): Flow<EventEnvelope>
    suspend fun channels(firmwareVersion: String? = null): List<MeshChannel> = emptyList()
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
