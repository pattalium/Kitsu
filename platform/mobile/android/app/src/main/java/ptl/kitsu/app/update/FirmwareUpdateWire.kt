package ptl.kitsu.app.update

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class FirmwareUpdateReceipt(
    val ok: Boolean,
    val protocol: Int,
    val state: String,
    @SerialName("update_id") val updateId: String? = null,
    @SerialName("firmware_version") val firmwareVersion: String,
    @SerialName("image_bytes") val imageBytes: Int,
    @SerialName("next_offset") val nextOffset: Int,
    @SerialName("chunk_bytes") val chunkBytes: Int,
    val resumed: Boolean,
    val replayed: Boolean,
    val scheduled: Boolean,
    val error: String? = null,
)

enum class FirmwareInstallStage {
    IDLE,
    IMPORTED,
    PREPARING,
    TRANSFERRING,
    VERIFYING,
    READY_TO_REBOOT,
    REBOOTING,
    COMPLETE,
    FAILED,
}

val FirmwareInstallStage.locksCompanionControls: Boolean
    get() = when (this) {
        FirmwareInstallStage.PREPARING,
        FirmwareInstallStage.TRANSFERRING,
        FirmwareInstallStage.VERIFYING,
        FirmwareInstallStage.READY_TO_REBOOT,
        FirmwareInstallStage.REBOOTING -> true
        else -> false
    }

data class FirmwareInstallProgress(
    val stage: FirmwareInstallStage = FirmwareInstallStage.IDLE,
    val firmwareVersion: String? = null,
    val bytesSent: Int = 0,
    val imageBytes: Int = 0,
    val errorCode: String? = null,
) {
    val percent: Int
        get() = if (imageBytes <= 0) 0 else ((bytesSent.toLong() * 100L) / imageBytes).toInt()
}
