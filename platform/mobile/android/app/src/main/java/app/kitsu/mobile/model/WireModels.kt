package app.kitsu.mobile.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.util.Base64
import java.util.UUID

const val WIRE_VERSION = 1
const val MAX_PAGE_SIZE = 100
const val MAX_MESSAGE_BYTES = 128

@Serializable
data class NeedLevels(
    val energy: Int = 0,
    val curiosity: Int = 0,
    val affection: Int = 0,
)

@Serializable
data class MeshState(
    val enabled: Boolean = false,
    val rxReady: Boolean = false,
    val timeValid: Boolean = false,
    val oneShotReady: Boolean = false,
)

@Serializable
data class MeshChannel(
    val slot: Int,
    val configured: Boolean,
    val name: String? = null,
)

@Serializable
data class MeshConfigurationReceipt(
    val enabled: Boolean,
    val profile: String,
    @SerialName("tx_power_dbm") val txPowerDbm: Int,
)

@Serializable
data class KitsuStatus(
    val deviceId: String,
    val companionName: String,
    val firmwareVersion: String? = null,
    val listening: Boolean = false,
    val mood: String = "UNKNOWN",
    val batteryPercent: Int? = null,
    val batteryMillivolts: Int? = null,
    val packReady: Boolean = false,
    val packId: String? = null,
    val packRevision: Long? = null,
    val bondLevel: Int = 0,
    val bondExperience: Int = 0,
    val bondProgressPercent: Int = 0,
    val evolutionStage: String? = null,
    val appearanceVariant: String? = null,
    val personality: String? = null,
    val unlockMask: Long = 0,
    val memoryCount: Int = 0,
    val needs: NeedLevels = NeedLevels(),
    val mesh: MeshState = MeshState(),
    val cursor: String? = null,
    val updatedAt: Long,
)

@Serializable
data class HistoryEntry(
    val id: String,
    val cursor: String,
    val kind: String,
    val summary: String,
    val occurredAt: Long,
)

@Serializable
data class HistoryPage(
    val items: List<HistoryEntry> = emptyList(),
    val cursor: String? = null,
    val hasMore: Boolean = false,
    val cursorExpired: Boolean = false,
)

@Serializable
data class Peer(
    val id: String,
    val name: String,
    val role: String = "client",
    val lastHeardAt: Long? = null,
    val route: String? = null,
)

@Serializable
data class PeerPage(val items: List<Peer> = emptyList())

@Serializable
data class Message(
    val id: String,
    val cursor: String,
    val direction: String,
    val peerId: String? = null,
    val channel: String? = null,
    val text: String,
    val state: String,
    val occurredAt: Long,
)

@Serializable
data class MessagePage(
    val items: List<Message> = emptyList(),
    val cursor: String? = null,
    val hasMore: Boolean = false,
    val cursorExpired: Boolean = false,
)

@Serializable
enum class ActionKind {
    @SerialName("pet") PET,
    @SerialName("feed") FEED,
    @SerialName("play") PLAY,
    @SerialName("listen_once") LISTEN_ONCE,
    @SerialName("send_message") SEND_MESSAGE,
}

@Serializable enum class MessageRoute { DIRECT, CHANNEL }

@Serializable
data class ActionCommand(
    val kind: ActionKind,
    val clientRequestId: String,
    val expiresInMs: Int = 30_000,
    val targetId: String? = null,
    val text: String? = null,
    val durationMs: Int? = null,
    val messageRoute: MessageRoute? = null,
)

@Serializable
data class ActionReceipt(
    @SerialName("action_id") val clientRequestId: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class ControllerForgetReceipt(
    val schema: String,
    val accepted: Boolean,
    val error: String? = null,
)

@Serializable
data class ActionApplyBody(
    @SerialName("action_id") val actionId: String,
    val kind: String,
    @SerialName("expires_at_epoch") val expiresAtEpoch: Long,
    val params: JsonObject,
)

@Serializable
data class EventEnvelope(
    val v: Int,
    val cursor: String,
    val kind: String,
    val body: JsonObject,
)

object ActionPolicy {
    private val meshChannelSlot = Regex("^[0-3]$")

    fun validate(command: ActionCommand): String? {
        if (runCatching { UUID.fromString(command.clientRequestId) }.isFailure) return "invalid_action_id"
        if (command.expiresInMs !in 1..120_000) return "invalid_expiry"
        return when (command.kind) {
            ActionKind.SEND_MESSAGE -> when {
                command.targetId.isNullOrBlank() -> "target_required"
                command.messageRoute == MessageRoute.DIRECT &&
                    !MeshPeerKeyPolicy.isCanonicalBase64Url(command.targetId) -> "invalid_peer_public_key"
                command.messageRoute == MessageRoute.CHANNEL &&
                    !meshChannelSlot.matches(command.targetId) -> "invalid_channel_slot"
                command.text.isNullOrBlank() -> "text_required"
                command.text.toByteArray(Charsets.UTF_8).size > MAX_MESSAGE_BYTES -> "text_too_long"
                command.messageRoute == null -> "route_required"
                command.durationMs != null -> "unexpected_arguments"
                else -> null
            }
            ActionKind.LISTEN_ONCE -> when {
                command.durationMs == null || command.durationMs !in 1_000..60_000 -> "invalid_duration"
                command.targetId != null || command.text != null || command.messageRoute != null ->
                    "unexpected_arguments"
                else -> null
            }
            else -> if (command.targetId != null || command.text != null || command.durationMs != null ||
                command.messageRoute != null
            ) "unexpected_arguments" else null
        }
    }
}

object MeshPeerKeyPolicy {
    private val base64Url = Regex("^[A-Za-z0-9_-]{43}$")

    fun canonicalBase64Url(value: String): String? {
        if (!base64Url.matches(value)) return null
        val decoded = runCatching { Base64.getUrlDecoder().decode(value) }.getOrNull() ?: return null
        return value.takeIf {
            decoded.size == 32 && Base64.getUrlEncoder().withoutPadding().encodeToString(decoded) == value
        }
    }

    fun isCanonicalBase64Url(value: String): Boolean = canonicalBase64Url(value) != null
}

fun ActionCommand.toDirectApplyBody(nowEpochSeconds: Long): ActionApplyBody {
    require(nowEpochSeconds in 1..UINT32_MAX) { "invalid_clock" }
    val ttlSeconds = ((expiresInMs.toLong() + 999L) / 1_000L).coerceIn(1L, 120L)
    val deadline = nowEpochSeconds + ttlSeconds
    require(deadline in 1..UINT32_MAX) { "invalid_expiry" }
    return ActionApplyBody(
        actionId = clientRequestId,
        kind = when (kind) {
            ActionKind.PET -> "pet"
            ActionKind.FEED -> "feed"
            ActionKind.PLAY -> "play"
            ActionKind.LISTEN_ONCE -> "listen_once"
            ActionKind.SEND_MESSAGE -> "send_message"
        },
        expiresAtEpoch = deadline,
        params = buildJsonObject {
            durationMs?.let { put("duration_ms", it) }
            messageRoute?.let { put("route", if (it == MessageRoute.DIRECT) "direct" else "channel") }
            targetId?.let { put("target_id", it) }
            text?.let { put("text", it) }
        },
    )
}

private const val UINT32_MAX = 4_294_967_295L
