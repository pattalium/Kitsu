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
    val txReady: Boolean = false,
    val timeValid: Boolean = false,
    val oneShotReady: Boolean = false,
)

@Serializable
data class MeshChannel(
    val slot: Int,
    /** Null means the backend has no authenticated device snapshot for this field. */
    val configured: Boolean? = null,
    /** Null means unknown. Clients must not invent a configured channel name. */
    val name: String? = null,
)

@Serializable
data class MeshConfigurationReceipt(
    val enabled: Boolean,
    val profile: String,
    @SerialName("tx_power_dbm") val txPowerDbm: Int,
)

@Serializable
data class LanState(
    val wifiConfigured: Boolean? = null,
    val wifiState: String = "unknown",
    val gatewayConfigured: Boolean? = null,
    val gatewayEnrolled: Boolean? = null,
    val lanState: String = "unknown",
    val gatewayEnrollmentState: String = "idle",
    val gatewayEnrollmentError: String? = null,
    val gatewayEnrollmentExpiresInMs: Int = 0,
    /** Device-reported application-security readiness; never inferred from eFuses. */
    val remoteConnectivityAllowed: Boolean? = null,
    /** Backend projection truth. Null on BLE and when the signed snapshot omitted it. */
    val online: Boolean? = null,
    val provenance: String? = null,
    val gatewayId: String? = null,
    val lastSeenAt: String? = null,
)

@Serializable
data class KitsuStatus(
    val protocol: Int = WIRE_VERSION,
    val deviceId: String,
    val displayName: String,
    val companionName: String? = null,
    val mood: String = "UNKNOWN",
    val batteryPercent: Int? = null,
    val needs: NeedLevels = NeedLevels(),
    val mesh: MeshState = MeshState(),
    val lan: LanState = LanState(),
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
    @SerialName("advertise_once") ADVERTISE_ONCE,
    @SerialName("send_message") SEND_MESSAGE,
    @SerialName("share_location_once") SHARE_LOCATION_ONCE,
}

@Serializable enum class MessageRoute { DIRECT, CHANNEL }
@Serializable enum class AdvertiseScope { NEARBY, MESH }
@Serializable enum class LocationExposure { NEARBY_ADVERT, MESH_ADVERT, MAP_CARD }

@Serializable
data class ActionCommand(
    val kind: ActionKind,
    val clientRequestId: String,
    val expiresInMs: Int = 30_000,
    val targetId: String? = null,
    val text: String? = null,
    val latE6: Int? = null,
    val lonE6: Int? = null,
    val durationMs: Int? = null,
    val messageRoute: MessageRoute? = null,
    val advertiseScope: AdvertiseScope? = null,
    val locationExposure: LocationExposure? = null,
)

@Serializable
data class ActionReceipt(
    @SerialName("action_id")
    val clientRequestId: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class WifiProvisioning(
    val ssid: String,
    val password: String,
    val security: WifiSecurity = WifiSecurity.WPA2_WPA3,
)

@Serializable
enum class WifiSecurity {
    @SerialName("wpa2") WPA2,
    @SerialName("wpa2_wpa3") WPA2_WPA3,
    @SerialName("wpa3") WPA3,
}

@Serializable
data class WifiConfigureBody(
    @SerialName("ssid_b64") val ssidB64: String,
    val security: WifiSecurity,
    val passphrase: String,
)

@Serializable
data class GatewayConfiguration(
    @SerialName("gateway_id") val gatewayId: String,
    val host: String,
    @SerialName("bootstrap_port") val bootstrapPort: Int,
    /** Steady-state private mTLS listener. */
    val port: Int,
    @SerialName("server_name") val serverName: String,
    @SerialName("ca_cert_der_b64") val caCertificateDerB64: String,
    @SerialName("spki_sha256_b64") val spkiSha256B64: String,
)

@Serializable
data class GatewayConfigurationReceipt(
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class OwnerEnrollmentView(
    val id: String,
    @SerialName("hardware_uid") val hardwareUid: String,
    @SerialName("display_name") val displayName: String,
    val status: String,
    @SerialName("expires_at") val expiresAt: String,
)

/** The claim token is one-use and must remain in-memory only for this handoff. */
data class OwnerEnrollmentChallenge(
    val enrollment: OwnerEnrollmentView,
    val claimToken: String,
)

@Serializable
data class GatewayEnrollmentBeginBody(
    val schema: String = "kitsu.gateway-enrollment.begin.v1",
    @SerialName("enrollment_id") val enrollmentId: String,
    @SerialName("claim_token") val claimToken: String,
)

@Serializable
data class GatewayEnrollmentFinishBody(
    val schema: String = "kitsu.gateway-enrollment.finish.v1",
    @SerialName("enrollment_id") val enrollmentId: String,
)

@Serializable
data class GatewayEnrollmentReceipt(
    val schema: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("enrollment_id") val enrollmentId: String,
    @SerialName("expires_in_ms") val expiresInMs: Int? = null,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class GatewayEnrollmentEvent(
    val schema: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("enrollment_id") val enrollmentId: String?,
    @SerialName("expires_in_ms") val expiresInMs: Int,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class ActionApplyBody(
    @SerialName("action_id") val actionId: String,
    val kind: String,
    @SerialName("expires_at_epoch") val expiresAtEpoch: Long,
    val params: JsonObject,
)

@Serializable
data class ProvisioningReceipt(
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

@Serializable
data class WifiRetryReceipt(
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
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
            command.latE6 != null || command.lonE6 != null -> "unexpected_location"
            command.durationMs != null || command.advertiseScope != null || command.locationExposure != null -> "unexpected_arguments"
            else -> null
        }
        ActionKind.SHARE_LOCATION_ONCE -> when {
            command.latE6 == null || command.lonE6 == null -> "location_required"
            command.latE6 !in -90_000_000..90_000_000 -> "latitude_out_of_range"
            command.lonE6 !in -180_000_000..180_000_000 -> "longitude_out_of_range"
            command.locationExposure == null -> "exposure_required"
            command.targetId != null || command.text != null -> "unexpected_message"
            command.durationMs != null || command.messageRoute != null || command.advertiseScope != null -> "unexpected_arguments"
            else -> null
        }
        ActionKind.LISTEN_ONCE -> when {
            command.durationMs == null || command.durationMs !in 1_000..60_000 -> "invalid_duration"
            command.hasAnyExcept("duration") -> "unexpected_arguments"
            else -> null
        }
        ActionKind.ADVERTISE_ONCE -> when {
            command.advertiseScope == null -> "scope_required"
            command.hasAnyExcept("scope") -> "unexpected_arguments"
            else -> null
        }
        else -> if (command.hasAnyExcept("none")) "unexpected_arguments" else null
        }
    }
}

/** Frozen MeshCore public-key representation used by firmware and backend.
 * Runtime/composer paths accept only canonical 32-byte unpadded base64url.
 * The 64-hex helper exists solely for explicitly labelled backend migration. */
object MeshPeerKeyPolicy {
    private val base64Url = Regex("^[A-Za-z0-9_-]{43}$")
    private val legacyHex = Regex("^[0-9A-Fa-f]{64}$")

    fun canonicalBase64Url(value: String): String? {
        if (!base64Url.matches(value)) return null
        val decoded = runCatching { Base64.getUrlDecoder().decode(value) }.getOrNull() ?: return null
        return value.takeIf {
            decoded.size == 32 && Base64.getUrlEncoder().withoutPadding().encodeToString(decoded) == value
        }
    }

    fun isCanonicalBase64Url(value: String): Boolean = canonicalBase64Url(value) != null

    fun migrateLegacyHex(value: String): String? {
        if (!legacyHex.matches(value)) return null
        val decoded = ByteArray(32) { index ->
            value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
        return Base64.getUrlEncoder().withoutPadding().encodeToString(decoded)
    }
}

private fun ActionCommand.hasAnyExcept(allowed: String): Boolean =
    (targetId != null || text != null || latE6 != null || lonE6 != null ||
        (durationMs != null && allowed != "duration") ||
        (messageRoute != null) ||
        (advertiseScope != null && allowed != "scope") ||
        locationExposure != null)

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
        ActionKind.SHARE_LOCATION_ONCE -> "share_location_once"
    },
    expiresAtEpoch = deadline,
    params = toApplyParameters(),
    )
}

fun ActionCommand.toApplyParameters(): JsonObject = buildJsonObject {
        durationMs?.let { put("duration_ms", it) }
        advertiseScope?.let { put("scope", if (it == AdvertiseScope.NEARBY) "nearby" else "mesh") }
        messageRoute?.let { put("route", if (it == MessageRoute.DIRECT) "direct" else "channel") }
        targetId?.let { put("target_id", it) }
        text?.let { put("text", it) }
        latE6?.let { put("lat_e6", it) }
        lonE6?.let { put("lon_e6", it) }
        locationExposure?.let {
            put(
                "exposure",
                when (it) {
                    LocationExposure.NEARBY_ADVERT -> "nearby_advert"
                    LocationExposure.MESH_ADVERT -> "mesh_advert"
                    LocationExposure.MAP_CARD -> "map_card"
                },
            )
        }
    }

fun WifiProvisioning.toConfigureBody(): WifiConfigureBody = WifiConfigureBody(
    ssidB64 = Base64.getUrlEncoder().withoutPadding().encodeToString(ssid.toByteArray(Charsets.UTF_8)),
    security = security,
    passphrase = password,
)

private const val UINT32_MAX = 4_294_967_295L
