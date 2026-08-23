package ptl.kitsu.app.model

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
data class LastFloodAdvert(
    /** MeshCore epoch reported by the authenticated Kitsu state response. */
    val emittedAt: Long,
    val state: String,
    /** Matching rebroadcast packet copies heard locally; never a unique-repeater count. */
    val repeatCount: Int? = null,
    val observationOpen: Boolean = false,
    /** Bounded distinct final path tokens from returned copies; never a repeater list. */
    val repeatSources: List<RepeatSource>? = null,
    val repeatSourcesTruncated: Boolean? = null,
)

@Serializable
data class LastNearbyAdvert(
    /** MeshCore epoch reported by the authenticated Kitsu state response. */
    val emittedAt: Long,
    val state: String,
)

@Serializable
data class MeshState(
    val enabled: Boolean = false,
    val rxReady: Boolean = false,
    val timeValid: Boolean = false,
    val oneShotReady: Boolean = false,
    val advertiseSupported: Boolean = false,
    val identityReady: Boolean = false,
    val advertiseReady: Boolean = false,
    val advertiseRetryAfterMs: Long = 0,
    val advertiseError: String? = null,
    /** Latest volatile Mesh-wide result. Nearby advertisements never replace it. */
    val lastFloodAdvert: LastFloodAdvert? = null,
    /** Latest volatile zero-hop Nearby result. Mesh-wide advertisements never replace it. */
    val lastNearbyAdvert: LastNearbyAdvert? = null,
)

@Serializable
enum class ChannelRegionScope {
    @SerialName("EU") EU,
}

@Serializable
data class MeshChannel(
    val slot: Int,
    val configured: Boolean,
    val name: String? = null,
    /** Null is the stock-compatible legacy flood; EU is an explicit #EU-scoped flood. */
    val regionScope: ChannelRegionScope? = null,
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
data class RepeatSource(
    /** Unauthenticated final path token observed on an exact returned packet copy. */
    val lastHopToken: String,
)

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
    val revision: String = id,
    /** Boot-scoped v2 journal namespace. Null only for legacy v1/cache entries. */
    val journalSession: String? = null,
    val senderName: String = "",
    /** Device-OLED unread state; firmware >=0.15 can clear it after an authenticated open-thread receipt. */
    val unreadOnKitsu: Boolean? = null,
    val route: String? = null,
    val localTx: String? = null,
    val deliveryAck: String? = null,
    /** Proven route relay count, never a count of radios that merely heard the packet. */
    val repeaterCount: Int? = null,
    /** Exact matching rebroadcast packet copies heard locally for an outbound channel send. */
    val repeatCount: Int? = null,
    /** Firmware v4 bounded-observation lifecycle. Null for v1-v3 and cached older rows. */
    val repeatObservationOpen: Boolean? = null,
    /** Bounded distinct final path tokens from returned copies; never a receiver/repeater list. */
    val repeatSources: List<RepeatSource>? = null,
    val repeatSourcesTruncated: Boolean? = null,
    val rssiDbm: Double? = null,
    val snrDb: Double? = null,
)

@Serializable
data class MessagePage(
    val items: List<Message> = emptyList(),
    val cursor: String? = null,
    val hasMore: Boolean = false,
    val cursorExpired: Boolean = false,
    val journalSession: String? = null,
    val journalRevision: String? = null,
    val protocolVersion: Int = 1,
)

@Serializable
data class MessageMarkReadReceipt(
    val schema: String,
    val accepted: Boolean,
    val error: String? = null,
    @SerialName("marked_count") val markedCount: Int,
    @SerialName("unchanged_count") val unchangedCount: Int,
    @SerialName("journal_session") val journalSession: String,
    @SerialName("journal_revision") val journalRevision: String,
)

@Serializable
enum class ActionKind {
    @SerialName("pet") PET,
    @SerialName("feed") FEED,
    @SerialName("play") PLAY,
    @SerialName("listen_once") LISTEN_ONCE,
    @SerialName("advertise_once") ADVERTISE_ONCE,
    @SerialName("send_message") SEND_MESSAGE,
}

@Serializable enum class MessageRoute { DIRECT, CHANNEL }

@Serializable
enum class AdvertiseScope {
    @SerialName("nearby") NEARBY,
    @SerialName("mesh") MESH,
}

@Serializable
data class ActionCommand(
    val kind: ActionKind,
    val clientRequestId: String,
    val expiresInMs: Int = 30_000,
    val targetId: String? = null,
    val text: String? = null,
    val durationMs: Int? = null,
    val messageRoute: MessageRoute? = null,
    val advertiseScope: AdvertiseScope? = null,
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
                command.durationMs != null || command.advertiseScope != null -> "unexpected_arguments"
                else -> null
            }
            ActionKind.LISTEN_ONCE -> when {
                command.durationMs == null || command.durationMs !in 1_000..60_000 -> "invalid_duration"
                command.targetId != null || command.text != null || command.messageRoute != null ||
                    command.advertiseScope != null ->
                    "unexpected_arguments"
                else -> null
            }
            ActionKind.ADVERTISE_ONCE -> when {
                command.advertiseScope == null -> "scope_required"
                command.targetId != null || command.text != null || command.durationMs != null ||
                    command.messageRoute != null -> "unexpected_arguments"
                else -> null
            }
            else -> if (command.targetId != null || command.text != null || command.durationMs != null ||
                command.messageRoute != null || command.advertiseScope != null
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
            ActionKind.ADVERTISE_ONCE -> "advertise_once"
            ActionKind.SEND_MESSAGE -> "send_message"
        },
        expiresAtEpoch = deadline,
        params = buildJsonObject {
            durationMs?.let { put("duration_ms", it) }
            messageRoute?.let { put("route", if (it == MessageRoute.DIRECT) "direct" else "channel") }
            targetId?.let { put("target_id", it) }
            text?.let { put("text", it) }
            advertiseScope?.let { put("scope", if (it == AdvertiseScope.NEARBY) "nearby" else "mesh") }
        },
    )
}

private const val UINT32_MAX = 4_294_967_295L
