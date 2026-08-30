package ptl.kitsu.app.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

const val COMPANION_PRESENTATION_OPEN_OPERATION = "companion.presentation.open.v1"
const val COMPANION_PRESENTATION_READ_OPERATION = "companion.presentation.read.v1"
const val COMPANION_PRESENTATION_CLOSE_OPERATION = "companion.presentation.close.v1"

@Serializable
enum class PetPresentationSurface {
    @SerialName("pet") PET,
    @SerialName("menu") MENU,
    @SerialName("connect") CONNECT,
    @SerialName("inbox") INBOX,
    @SerialName("game_menu") GAME_MENU,
    @SerialName("game") GAME,
    @SerialName("listen") LISTEN,
    @SerialName("sleep") SLEEP,
    @SerialName("status") STATUS,
    @SerialName("pair_phone") PAIR_PHONE,
    @SerialName("controller_manager") CONTROLLER_MANAGER,
    @SerialName("controller_confirm") CONTROLLER_CONFIRM,
    @SerialName("controller_result") CONTROLLER_RESULT,
    @SerialName("wild_encounter") WILD_ENCOUNTER,
    @SerialName("field_guide") FIELD_GUIDE,
    @SerialName("goals") GOALS,
    @SerialName("clock") CLOCK,
    @SerialName("adventure") ADVENTURE,
    @SerialName("activity") ACTIVITY,
    @SerialName("unknown") UNKNOWN,
}

@Serializable
enum class PetPresentationRole {
    @SerialName("idle") IDLE,
    @SerialName("blink") BLINK,
    @SerialName("pet") PET,
    @SerialName("sleep") SLEEP,
    @SerialName("listen") LISTEN,
    @SerialName("surprise") SURPRISE,
    @SerialName("play") PLAY,
    @SerialName("tired") TIRED,
    @SerialName("feed") FEED,
    @SerialName("wake") WAKE,
    @SerialName("meet") MEET,
    @SerialName("evolve") EVOLVE,
    @SerialName("unknown") UNKNOWN,
}

@Serializable
enum class PetPresentationPlayback {
    @SerialName("hold") HOLD,
    @SerialName("once") ONCE,
    @SerialName("loop") LOOP,
    @SerialName("ping_pong") PING_PONG,
    @SerialName("unknown") UNKNOWN,
}

@Serializable
data class PetPresentationPack(
    val valid: Boolean,
    val name: String,
    val id: Long,
    val revision: Long,
    @SerialName("total_bytes") val totalBytes: Long,
    @SerialName("payload_crc32") val payloadCrc32: Long,
    @SerialName("header_crc32") val headerCrc32: Long,
    val format: Int,
    val width: Int,
    val height: Int,
    @SerialName("frame_count") val frameCount: Int,
    val appearance: Int,
)

@Serializable
data class PetPresentationAnimation(
    val active: Boolean,
    val finite: Boolean,
    @SerialName("requested_role") val requestedRole: PetPresentationRole,
    @SerialName("resolved_role") val resolvedRole: PetPresentationRole,
    val playback: PetPresentationPlayback,
    val token: Long,
    @SerialName("elapsed_ms") val elapsedMs: Long,
)

@Serializable
data class PetPresentationFrame(
    val available: Boolean,
    val encoding: String,
    val bytes: Int,
    val sha256: String,
)

@Serializable
data class PetPresentationState(
    val ok: Boolean,
    val schema: Int,
    @SerialName("session_id") val sessionId: Long,
    @SerialName("captured_at_ms") val capturedAtMs: Long,
    val surface: PetPresentationSurface,
    @SerialName("display_awake") val displayAwake: Boolean,
    @SerialName("frame_visible") val frameVisible: Boolean,
    val pack: PetPresentationPack,
    val animation: PetPresentationAnimation,
    val frame: PetPresentationFrame,
)

data class PetPresentationChunk(
    val sessionId: Long,
    val offset: Int,
    val nextOffset: Int,
    val complete: Boolean,
    val frameSha256: String,
    val data: ByteArray,
)

data class PetPresentationSnapshot(
    val state: PetPresentationState,
    val frame: ByteArray?,
) {
    init {
        require((frame != null) == state.frame.available) { "presentation_frame_presence_mismatch" }
        require(frame == null || frame.size == state.frame.bytes) { "presentation_frame_size_mismatch" }
    }
}
