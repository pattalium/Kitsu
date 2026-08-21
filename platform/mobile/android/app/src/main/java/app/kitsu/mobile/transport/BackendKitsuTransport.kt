package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshState
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshPeerKeyPolicy
import app.kitsu.mobile.model.NeedLevels
import app.kitsu.mobile.model.OwnerEnrollmentChallenge
import app.kitsu.mobile.model.OwnerEnrollmentView
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.WIRE_VERSION
import app.kitsu.mobile.model.toApplyParameters
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.relay.MAX_RELAY_ENROLLMENT_REQUEST_BYTES
import app.kitsu.mobile.relay.MAX_RELAY_FRAME_BYTES
import app.kitsu.mobile.relay.MobileRelayBackend
import app.kitsu.mobile.relay.MobileRelayBindingRequest
import app.kitsu.mobile.relay.MobileRelayClaimResponse
import app.kitsu.mobile.relay.MobileRelayEnvelopeAccepted
import app.kitsu.mobile.relay.MobileRelayHttpRoutes
import app.kitsu.mobile.relay.MobileRelayIdentity
import app.kitsu.mobile.relay.MobileRelayWirePolicy
import java.io.IOException
import java.time.Instant
import java.util.UUID
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString

interface AccessTokenProvider {
    suspend fun accessToken(): String?
}

/** Canonical owner routes. Templates remain injectable for self-hosted backends. */
data class BackendRoutes(
    val companions: String = "/v1/companions",
    val enrollments: String = "/v1/enrollments",
    val gateways: String = "/v1/gateways",
    val snapshot: String = "/v1/companions/{id}/snapshot",
    val peers: String = "/v1/companions/{id}/peers",
    val channels: String = "/v1/companions/{id}/channels",
    val events: String = "/v1/companions/{id}/events",
    val actions: String = "/v1/companions/{id}/actions",
)

data class BackendConfiguration(
    val baseUrl: String,
    val routes: BackendRoutes = BackendRoutes(),
    val mobileRelayRoutes: MobileRelayHttpRoutes = MobileRelayHttpRoutes(),
) {
    val parsedBaseUrl: HttpUrl = baseUrl.toHttpUrl().also { url ->
        require(url.scheme == "https") { "backend_requires_https" }
        require(url.username.isEmpty() && url.password.isEmpty()) { "credentials_forbidden_in_url" }
        require(url.query == null && url.fragment == null) { "backend_base_url_must_not_have_query" }
    }
}

@Serializable
data class RemoteCompanion(
    val id: String,
    @SerialName("hardware_uid") val hardwareUid: String,
    @SerialName("display_name") val displayName: String,
    val status: String,
    @SerialName("last_seen_at") val lastSeenAt: String? = null,
)

/**
 * Public, owner-authorized gateway trust record returned by api.k32.run.
 * The CA and pin are deliberately never rendered by the normal UI; they are
 * carried directly into the authenticated BLE gateway.configure operation.
 */
@Serializable
data class GatewayProvisioningRecord(
    @SerialName("gateway_id") val gatewayId: String,
    @SerialName("display_name") val displayName: String,
    val host: String,
    @SerialName("bootstrap_port") val bootstrapPort: Int,
    val port: Int,
    @SerialName("server_name") val serverName: String,
    @SerialName("ca_cert_der_b64") val caCertificateDerB64: String,
    @SerialName("spki_sha256_b64") val spkiSha256B64: String,
    val state: String,
) {
    fun toGatewayConfiguration(): GatewayConfiguration = GatewayConfiguration(
        gatewayId = gatewayId,
        host = host,
        bootstrapPort = bootstrapPort,
        port = port,
        serverName = serverName,
        caCertificateDerB64 = caCertificateDerB64,
        spkiSha256B64 = spkiSha256B64,
    )
}

interface RemoteCompanionSelectionStore {
    fun selectedCompanionId(): String?
    fun saveSelectedCompanionId(value: String?)
}

interface RemoteCompanionCatalog {
    suspend fun companions(): List<RemoteCompanion>
    fun selectedCompanionId(): String?
    suspend fun selectCompanion(id: String)
}

interface GatewayCatalogService {
    suspend fun gateways(): List<GatewayProvisioningRecord>
}

interface OwnerEnrollmentService {
    suspend fun createEnrollment(hardwareUid: String, displayName: String): OwnerEnrollmentChallenge
}

@Serializable
private data class CreateEnrollmentRequest(
    @SerialName("hardware_uid") val hardwareUid: String,
    @SerialName("display_name") val displayName: String,
)

@Serializable
private data class CreateEnrollmentResponse(
    val enrollment: OwnerEnrollmentView,
    @SerialName("claim_token") val claimToken: String,
)

@Serializable
internal data class ListEnvelope<T>(val items: T)

internal object GatewayCatalogPolicy {
    const val OFFICIAL_HOST = "api.k32.run"
    const val GATEWAY_SETUP_URL = "https://docs.k32.run/connectivity/"
    private val stateCode = Regex("^[a-z][a-z0-9_]{0,63}$")

    fun isTrustedOrigin(url: HttpUrl): Boolean =
        url.scheme == "https" && url.host == OFFICIAL_HOST && url.port == 443

    fun validate(record: GatewayProvisioningRecord): String? {
        if (record.displayName.toByteArray(Charsets.UTF_8).size !in 1..80 ||
            record.displayName.any(Char::isISOControl)
        ) return "invalid_gateway_display_name"
        if (!stateCode.matches(record.state)) return "invalid_gateway_state"
        return ProvisioningPolicy.gatewayError(record.toGatewayConfiguration())
    }
}

@Serializable
internal data class BackendSnapshot(
    val companion: JsonObject,
    val vitals: JsonObject = JsonObject(emptyMap()),
    val mood: JsonObject = JsonObject(emptyMap()),
    val bond: JsonObject = JsonObject(emptyMap()),
    val evolution: JsonObject = JsonObject(emptyMap()),
    val connectivity: JsonObject = JsonObject(emptyMap()),
    val mesh: JsonObject = JsonObject(emptyMap()),
    val counts: JsonObject = JsonObject(emptyMap()),
    @SerialName("recent_events") val recentEvents: List<BackendEvent> = emptyList(),
    val cursor: String,
)

@Serializable
internal data class BackendEvent(
    val cursor: String,
    @SerialName("event_id") val eventId: String,
    @SerialName("event_type") val eventType: String,
    @SerialName("observed_epoch") val observedEpoch: Long? = null,
    @SerialName("boot_id") val bootId: Long? = null,
    @SerialName("monotonic_ms") val monotonicMs: Long? = null,
    val body: JsonObject = JsonObject(emptyMap()),
    @SerialName("received_at") val receivedAt: String,
)

@Serializable
internal data class BackendChannel(
    val slot: Int,
    val configured: Boolean? = null,
    val name: String? = null,
    @SerialName("max_utf8_bytes") val maxUtf8Bytes: Int,
)

@Serializable
internal data class RemoteActionRequest(
    @SerialName("action_type") val actionType: String,
    val parameters: JsonObject,
    @SerialName("expires_in_seconds") val expiresInSeconds: Int,
)

@Serializable
private data class RemoteActionView(
    val id: String,
    val status: String,
    @SerialName("action_type") val actionType: String,
)

@Serializable
private data class ApiErrorBody(val code: String = "request_failed")

@Serializable
private data class ApiErrorEnvelope(val error: ApiErrorBody? = null)

internal object BackendWireMapper {
    fun status(snapshot: BackendSnapshot): KitsuStatus {
        val companion = snapshot.companion
        val vitals = snapshot.vitals
        val needs = vitals.objectOrNull("needs") ?: vitals
        val updatedAt = epoch(snapshot.connectivity["last_seen_at"])
            ?: snapshot.recentEvents.maxOfOrNull { eventEpoch(it) }
            ?: 0L
        return KitsuStatus(
            deviceId = companion.string("hardware_uid") ?: companion.string("id") ?: "unknown",
            displayName = companion.string("display_name") ?: "Kitsu",
            companionName = snapshot.evolution.string("pack_name")
                ?: snapshot.evolution.string("companion_name"),
            mood = snapshot.mood.string("state")
                ?: snapshot.mood.string("name")
                ?: snapshot.mood.string("mood")
                ?: "UNKNOWN",
            batteryPercent = vitals.integer("battery_percent") ?: vitals.integer("batteryPercent"),
            needs = NeedLevels(
                energy = needs.integer("energy") ?: 0,
                curiosity = needs.integer("curiosity") ?: 0,
                affection = needs.integer("affection") ?: 0,
            ),
            mesh = MeshState(
                enabled = snapshot.mesh.bool("enabled") ?: false,
                rxReady = snapshot.mesh.bool("rx_ready") ?: snapshot.mesh.bool("rxReady") ?: false,
                txReady = snapshot.mesh.bool("tx_ready") ?: snapshot.mesh.bool("txReady") ?: false,
                timeValid = snapshot.mesh.bool("time_valid") ?: false,
                oneShotReady = snapshot.mesh.bool("one_shot_ready") ?: false,
            ),
            lan = LanState(
                wifiConfigured = snapshot.connectivity.bool("wifi_configured"),
                wifiState = snapshot.connectivity.string("wifi_state") ?: "unknown",
                gatewayConfigured = snapshot.connectivity.bool("gateway_configured"),
                gatewayEnrolled = snapshot.connectivity.bool("gateway_enrolled"),
                lanState = snapshot.connectivity.string("lan_state") ?: "unknown",
                gatewayEnrollmentState = snapshot.connectivity.string("gateway_enrollment_state") ?: "idle",
                gatewayEnrollmentError = snapshot.connectivity.string("gateway_enrollment_error"),
                gatewayEnrollmentExpiresInMs = snapshot.connectivity.integer(
                    "gateway_enrollment_expires_in_ms",
                ) ?: 0,
                remoteConnectivityAllowed = snapshot.connectivity.bool("remote_connectivity_allowed"),
                online = snapshot.connectivity.bool("online"),
                provenance = snapshot.connectivity.string("provenance"),
                gatewayId = snapshot.connectivity.string("gateway_id"),
                lastSeenAt = snapshot.connectivity.string("last_seen_at"),
            ),
            cursor = snapshot.cursor,
            updatedAt = updatedAt,
        )
    }

    fun history(events: List<BackendEvent>): HistoryPage = HistoryPage(
        items = events.map { event ->
            HistoryEntry(
                id = event.eventId,
                cursor = event.cursor,
                kind = event.eventType,
                summary = event.body.string("summary")
                    ?: event.body.string("text")
                    ?: event.eventType.replace('.', ' '),
                occurredAt = eventEpoch(event),
            )
        },
        cursor = events.lastOrNull()?.cursor,
        hasMore = events.size >= 100,
    )

    fun messages(events: List<BackendEvent>): MessagePage {
        val messages = events.filter { it.eventType.startsWith("mesh.message") }.map { event ->
            val direction = event.body.string("direction") ?: when {
                event.eventType.contains("received") || event.eventType.endsWith(".rx") -> "inbound"
                else -> "outbound"
            }
            val route = event.body.string("route") ?: event.body.string("kind")
            val target = event.body.string("target_id") ?: event.body.string("target")
            val channel = event.body.string("channel")
                ?: target.takeIf { route == "channel" }
            if (channel != null && !Regex("^[0-3]$").matches(channel)) {
                throw TransportException("malformed_channel_slot")
            }
            val peerReference = event.body.string("peer_id")
                ?: event.body.string("source_id")
                ?: target.takeIf { route != "channel" }
            Message(
                id = event.eventId,
                cursor = event.cursor,
                direction = direction,
                peerId = canonicalPeerKeyOrNull(peerReference),
                channel = channel,
                text = event.body.string("text") ?: "",
                state = event.body.string("state") ?: if (direction == "inbound") "received" else "sent",
                occurredAt = eventEpoch(event),
            )
        }
        return MessagePage(
            items = messages,
            cursor = events.lastOrNull()?.cursor,
            hasMore = events.size >= 100,
        )
    }

    fun peers(items: List<JsonObject>): PeerPage = PeerPage(items.map { item ->
        val canonical = item.string("public_key_b64")?.let { preferred ->
            MeshPeerKeyPolicy.canonicalBase64Url(preferred)
                ?: throw TransportException("malformed_peer_public_key")
        }
        val migrated = item.string("public_key_hex")?.let { legacy ->
            MeshPeerKeyPolicy.migrateLegacyHex(legacy)
                ?: throw TransportException("malformed_peer_public_key")
        }
        if (canonical == null && migrated == null) throw TransportException("missing_peer_public_key")
        if (canonical != null && migrated != null && canonical != migrated) {
            throw TransportException("peer_public_key_mismatch")
        }
        Peer(
            id = canonical ?: migrated!!,
            name = item.string("name")?.takeIf { it.isNotBlank() } ?: "Unknown peer",
            role = item.string("role") ?: "client",
            lastHeardAt = item.objectOrNull("last_seen")?.long("epoch"),
            route = item.objectOrNull("signal")?.string("scope"),
        )
    })

    fun channels(items: List<BackendChannel>): List<MeshChannel> {
        if (items.size != 4 || items.map { it.slot }.toSet() != setOf(0, 1, 2, 3) ||
            items.any { channel ->
                channel.slot !in 0..3 || channel.maxUtf8Bytes != 128 ||
                    !validChannelMetadata(channel.configured, channel.name)
            }
        ) {
            throw TransportException("malformed_channels")
        }
        return items.sortedBy { it.slot }
            .map { MeshChannel(it.slot, it.configured, it.name) }
    }

    private fun validChannelMetadata(configured: Boolean?, name: String?): Boolean = when {
        configured == true && name == null -> false
        configured != true && name != null -> false
        name == null -> true
        name.toByteArray(Charsets.UTF_8).size !in 1..32 -> false
        name.any { it.isISOControl() || Character.isSurrogate(it) } -> false
        else -> true
    }

    fun event(event: BackendEvent): EventEnvelope = EventEnvelope(
        v = WIRE_VERSION,
        cursor = event.cursor,
        kind = event.eventType,
        body = event.body,
    )

    fun remoteAction(command: ActionCommand): RemoteActionRequest {
        command.requireAllowed()
        val parameters = command.toApplyParameters()
        val (type, actionParameters) = when (command.kind) {
            ActionKind.PET -> "companion.pet" to JsonObject(emptyMap())
            ActionKind.FEED -> "companion.feed" to JsonObject(emptyMap())
            ActionKind.PLAY -> "companion.play" to JsonObject(emptyMap())
            ActionKind.LISTEN_ONCE -> "companion.listen_once" to parameters
            ActionKind.ADVERTISE_ONCE -> "mesh.introduce" to parameters
            ActionKind.SEND_MESSAGE -> "message.send" to buildJsonObject {
                put("route", parameters.getValue("route"))
                put("target", parameters.getValue("target_id"))
                put("text", parameters.getValue("text"))
            }
            ActionKind.SHARE_LOCATION_ONCE -> throw TransportException("remote_action_unavailable")
        }
        return RemoteActionRequest(
            actionType = type,
            parameters = actionParameters,
            expiresInSeconds = ((command.expiresInMs + 999) / 1_000).coerceIn(5, 120),
        )
    }

    fun chooseCompanion(items: List<RemoteCompanion>, selectedId: String?): RemoteCompanion? {
        val selected = selectedId?.takeIf(::isUuid)?.let { id -> items.firstOrNull { it.id == id } }
        return selected ?: items.singleOrNull()
    }

    private fun canonicalPeerKeyOrNull(value: String?): String? {
        if (value == null) return null
        return MeshPeerKeyPolicy.canonicalBase64Url(value)
            ?: MeshPeerKeyPolicy.migrateLegacyHex(value)
            ?: throw TransportException("malformed_peer_public_key")
    }

    private fun eventEpoch(event: BackendEvent): Long =
        event.observedEpoch ?: epoch(JsonPrimitive(event.receivedAt)) ?: 0L

    private fun epoch(value: JsonElement?): Long? {
        val primitive = value as? JsonPrimitive ?: return null
        primitive.longOrNull?.let { return it }
        return primitive.contentOrNull?.let { runCatching { Instant.parse(it).epochSecond }.getOrNull() }
    }

    private fun JsonObject.objectOrNull(name: String): JsonObject? =
        runCatching { get(name)?.jsonObject }.getOrNull()

    private fun JsonObject.string(name: String): String? =
        runCatching { get(name)?.jsonPrimitive?.contentOrNull }.getOrNull()

    private fun JsonObject.integer(name: String): Int? =
        runCatching { get(name)?.jsonPrimitive?.intOrNull }.getOrNull()

    private fun JsonObject.long(name: String): Long? =
        runCatching { get(name)?.jsonPrimitive?.longOrNull }.getOrNull()

    private fun JsonObject.bool(name: String): Boolean? =
        runCatching { get(name)?.jsonPrimitive?.booleanOrNull }.getOrNull()
}

internal object RemoteSnapshotPolicy {
    const val AUTHENTICATED_PROVENANCE = "gateway_mtls_device_hmac"
    private const val MIN_REASONABLE_EPOCH = 1_577_836_800L // 2020-01-01
    private const val MAX_REASONABLE_EPOCH = 4_102_444_800L // 2100-01-01

    fun validationError(
        status: KitsuStatus,
        expectedDeviceId: String? = null,
        expectedGatewayId: String? = null,
    ): String? {
        if (expectedDeviceId != null && status.deviceId != expectedDeviceId) {
            return "remote_companion_binding_failed"
        }
        if (status.lan.online != true) return "remote_companion_offline"
        if (status.lan.provenance != AUTHENTICATED_PROVENANCE) return "remote_provenance_unverified"
        val gatewayId = status.lan.gatewayId
        if (gatewayId == null || !isCanonicalLowercaseUuid(gatewayId)) return "remote_gateway_unverified"
        if (expectedGatewayId != null && gatewayId != expectedGatewayId) return "remote_gateway_binding_failed"
        val lastSeenAt = status.lan.lastSeenAt
        val epoch = lastSeenAt?.let { runCatching { Instant.parse(it).epochSecond }.getOrNull() }
        if (epoch == null || epoch !in MIN_REASONABLE_EPOCH..MAX_REASONABLE_EPOCH) {
            return "remote_last_seen_invalid"
        }
        return null
    }
}

class BackendKitsuTransport(
    private val configuration: BackendConfiguration,
    private val tokens: AccessTokenProvider,
    private val selection: RemoteCompanionSelectionStore,
    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .build(),
) : KitsuTransport, RemoteCompanionCatalog, GatewayCatalogService, OwnerEnrollmentService,
    MobileRelayBackend {
    override val mode: ConnectionMode = ConnectionMode.REMOTE_BACKEND
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }
    @Volatile private var activeCompanionId: String? = null

    override suspend fun connect(): ConnectResult {
        if (tokens.accessToken().isNullOrBlank()) return ConnectResult.Failed("sign_in_required")
        return try {
            val available = companions()
            val selected = BackendWireMapper.chooseCompanion(available, selection.selectedCompanionId())
            when {
                available.isEmpty() -> {
                    activeCompanionId = null
                    selection.saveSelectedCompanionId(null)
                    ConnectResult.Failed("no_remote_companion")
                }
                selected == null -> {
                    activeCompanionId = null
                    selection.saveSelectedCompanionId(null)
                    ConnectResult.Failed("companion_selection_required")
                }
                else -> {
                    activeCompanionId = selected.id
                    selection.saveSelectedCompanionId(selected.id)
                    status()
                    ConnectResult.Connected
                }
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: TransportException) {
            ConnectResult.Failed(failure.code)
        } catch (failure: Throwable) {
            SafeLog.warn("backend_connect", "backend_unavailable", failure)
            ConnectResult.Failed("backend_unavailable")
        }
    }

    override suspend fun disconnect() {
        activeCompanionId = null
    }

    override suspend fun companions(): List<RemoteCompanion> {
        val items = get<ListEnvelope<List<RemoteCompanion>>>(configuration.routes.companions).items
        if (items.any { !isUuid(it.id) }) throw TransportException("malformed_response")
        return items
    }

    override suspend fun gateways(): List<GatewayProvisioningRecord> {
        if (!GatewayCatalogPolicy.isTrustedOrigin(configuration.parsedBaseUrl)) {
            throw TransportException("untrusted_gateway_catalog_origin")
        }
        val items = get<ListEnvelope<List<GatewayProvisioningRecord>>>(configuration.routes.gateways).items
        if (items.any { GatewayCatalogPolicy.validate(it) != null } ||
            items.map { it.gatewayId }.distinct().size != items.size
        ) {
            throw TransportException("malformed_gateway_catalog")
        }
        return items
    }

    override fun selectedCompanionId(): String? = selection.selectedCompanionId()

    override suspend fun selectCompanion(id: String) {
        if (!isUuid(id)) throw TransportException("invalid_companion_id")
        val match = companions().firstOrNull { it.id == id } ?: throw TransportException("companion_not_found")
        selection.saveSelectedCompanionId(match.id)
        activeCompanionId = match.id
    }

    override suspend fun createEnrollment(
        hardwareUid: String,
        displayName: String,
    ): OwnerEnrollmentChallenge {
        EnrollmentPolicy.ownerCreateError(hardwareUid, displayName)?.let { throw TransportException(it) }
        val response: CreateEnrollmentResponse = post(
            configuration.routes.enrollments,
            json.encodeToString(CreateEnrollmentRequest(hardwareUid, displayName)),
        )
        val enrollment = response.enrollment
        if (!EnrollmentPolicy.canonicalUuid(enrollment.id) ||
            enrollment.hardwareUid != hardwareUid || enrollment.displayName != displayName ||
            enrollment.status != "pending" ||
            runCatching { Instant.parse(enrollment.expiresAt) }.getOrNull() == null ||
            !EnrollmentPolicy.canonicalBase64Url(response.claimToken, 32)
        ) {
            throw TransportException("malformed_enrollment_response")
        }
        return OwnerEnrollmentChallenge(enrollment, response.claimToken)
    }

    override suspend fun createEnrollment(
        installationId: String,
        hardwareUid: String,
        displayName: String,
    ): OwnerEnrollmentChallenge {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId)) {
            throw TransportException("invalid_mobile_relay_identity")
        }
        return createEnrollment(hardwareUid, displayName)
    }

    override suspend fun ensureRelay(installationId: String, gatewayId: String): MobileRelayIdentity {
        if (!isCanonicalLowercaseUuid(installationId) || !isCanonicalLowercaseUuid(gatewayId)) {
            throw TransportException("invalid_mobile_relay_identity")
        }
        val identity: MobileRelayIdentity = put(
            mobileRelayRoute(configuration.mobileRelayRoutes.binding, installationId),
            json.encodeToString(MobileRelayBindingRequest(gatewayId)),
        )
        if (identity.installationId != installationId || identity.gatewayId != gatewayId ||
            runCatching { Instant.parse(identity.createdAt) }.getOrNull() == null ||
            runCatching {
                MobileRelayWirePolicy.decodeCanonical(identity.caCertificateDerB64, 8 * 1024).isNotEmpty()
            }.getOrDefault(false).not()
        ) throw TransportException("mobile_relay_binding_failed")
        return identity
    }

    override suspend fun claimEnrollment(
        installationId: String,
        enrollmentId: String,
        exactRequest: ByteArray,
    ): ByteArray {
        if (!isCanonicalLowercaseUuid(installationId) || !isCanonicalLowercaseUuid(enrollmentId) ||
            exactRequest.isEmpty() || exactRequest.size > MAX_RELAY_ENROLLMENT_REQUEST_BYTES
        ) throw TransportException("invalid_mobile_relay_claim")
        val response = postExact(
            mobileRelayRoute(configuration.mobileRelayRoutes.claim, installationId, enrollmentId),
            exactRequest,
        )
        val parsed = runCatching {
            json.decodeFromString(MobileRelayClaimResponse.serializer(), response.toString(Charsets.UTF_8))
        }.getOrElse { throw TransportException("malformed_enrollment_response", it) }
        if (!isCanonicalLowercaseUuid(parsed.companionId) ||
            !isCanonicalLowercaseUuid(parsed.gatewayId) || parsed.keyVersion <= 0
        ) throw TransportException("malformed_enrollment_response")
        return response
    }

    override suspend fun uploadEnvelope(
        installationId: String,
        spoolRecordId: String,
        exactEnvelope: ByteArray,
    ): ByteArray {
        if (!isCanonicalLowercaseUuid(installationId) ||
            !MobileRelayWirePolicy.canonicalU64(spoolRecordId) ||
            exactEnvelope.isEmpty() || exactEnvelope.size > MAX_RELAY_FRAME_BYTES
        ) throw TransportException("invalid_mobile_relay_envelope")
        val response = postExact(
            mobileRelayRoute(configuration.mobileRelayRoutes.envelopes, installationId),
            exactEnvelope,
            mapOf("X-Kitsu-Spool-Record-Id" to spoolRecordId),
        )
        val accepted = runCatching {
            json.decodeFromString(MobileRelayEnvelopeAccepted.serializer(), response.toString(Charsets.UTF_8))
        }.getOrElse { throw TransportException("malformed_envelope_ack", it) }
        if (!accepted.accepted || accepted.spoolRecordId != spoolRecordId ||
            accepted.spoolRecordId == "0" || accepted.sequence == "0" ||
            !MobileRelayWirePolicy.canonicalU64(accepted.sequence)
        ) throw TransportException("malformed_envelope_ack")
        return MobileRelayWirePolicy.gatewayAcknowledgement(
            accepted.spoolRecordId,
            accepted.sequence,
        )
    }

    override fun downlinks(installationId: String): Flow<ByteArray> = callbackFlow {
        if (!isCanonicalLowercaseUuid(installationId)) {
            close(TransportException("invalid_mobile_relay_identity"))
            return@callbackFlow
        }
        val token = tokens.accessToken()
        if (token.isNullOrBlank()) {
            close(TransportException("sign_in_required"))
            return@callbackFlow
        }
        val request = Request.Builder()
            .url(
                endpoint(mobileRelayRoute(configuration.mobileRelayRoutes.session, installationId))
                    .newBuilder().scheme("wss").build(),
            )
            .header("Authorization", "Bearer $token")
            .build()
        val socket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) =
                accept(webSocket, text.toByteArray())
            override fun onMessage(webSocket: WebSocket, bytes: ByteString) =
                accept(webSocket, bytes.toByteArray())

            private fun accept(webSocket: WebSocket, bytes: ByteArray) {
                if (bytes.isEmpty() || bytes.size > MAX_RELAY_FRAME_BYTES) {
                    webSocket.close(1009, "relay_downlink_size")
                    close(TransportException("relay_downlink_size"))
                } else if (!trySend(bytes).isSuccess) {
                    webSocket.close(1011, "relay_queue_unavailable")
                    close(TransportException("relay_queue_unavailable"))
                }
            }

            override fun onFailure(webSocket: WebSocket, failure: Throwable, response: Response?) {
                close(TransportException("mobile_relay_session_failed", failure))
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                close()
            }
        })
        awaitClose { socket.close(1000, "relay_stop") }
    }

    override suspend fun status(): KitsuStatus = BackendWireMapper.status(
        get(route(configuration.routes.snapshot)),
    ).also { status ->
        RemoteSnapshotPolicy.validationError(status)?.let { throw TransportException(it) }
    }

    override suspend fun history(after: String?, limit: Int): HistoryPage {
        val bounded = boundedLimit(limit)
        val events = fetchEvents(after, bounded)
        return BackendWireMapper.history(events).copy(hasMore = events.size >= bounded)
    }

    override suspend fun peers(): PeerPage = BackendWireMapper.peers(
        get<ListEnvelope<List<JsonObject>>>(route(configuration.routes.peers)).items,
    )

    override suspend fun channels(): List<MeshChannel> = BackendWireMapper.channels(
        get<ListEnvelope<List<BackendChannel>>>(route(configuration.routes.channels)).items,
    )

    override suspend fun messages(after: String?, limit: Int): MessagePage {
        val bounded = boundedLimit(limit)
        val events = fetchEvents(after, bounded)
        return BackendWireMapper.messages(events).copy(hasMore = events.size >= bounded)
    }

    override suspend fun action(command: ActionCommand): ActionReceipt {
        val request = BackendWireMapper.remoteAction(command)
        val view: RemoteActionView = post(
            route(configuration.routes.actions),
            json.encodeToString(request),
            mapOf("Idempotency-Key" to command.clientRequestId),
        )
        return ActionReceipt(
            clientRequestId = command.clientRequestId,
            accepted = view.status !in setOf("rejected", "failed", "expired"),
            state = view.status,
        )
    }

    override fun events(after: String?): Flow<EventEnvelope> = flow {
        var cursor = after
        var retryMillis = 1_000L
        while (currentCoroutineContext().isActive) {
            try {
                val next = fetchEvents(cursor, 100)
                retryMillis = 1_000L
                if (next.isEmpty()) {
                    delay(EVENT_POLL_MILLIS)
                } else {
                    next.forEach { emit(BackendWireMapper.event(it)) }
                    cursor = next.last().cursor
                }
            } catch (failure: TransportException) {
                if (failure.code == "sign_in_required") throw failure
                SafeLog.warn("backend_events", "poll_failed", failure)
                delay(retryMillis)
                retryMillis = (retryMillis * 2).coerceAtMost(30_000L)
            }
        }
    }

    private suspend fun fetchEvents(after: String?, limit: Int): List<BackendEvent> =
        get<ListEnvelope<List<BackendEvent>>>(
            route(configuration.routes.events),
            mapOf("after" to validatedCursor(after), "limit" to boundedLimit(limit).toString()),
        ).items

    private fun validatedCursor(value: String?): String? {
        if (value == null) return null
        if (value.isEmpty() || value.toByteArray(Charsets.UTF_8).size > 128 || value.any { it.isISOControl() }) {
            throw TransportException("invalid_cursor")
        }
        return value
    }

    private fun route(template: String): String {
        val id = activeCompanionId ?: selection.selectedCompanionId()
            ?.takeIf(::isUuid)
            ?: throw TransportException("companion_selection_required")
        return template.replace("{id}", id)
    }

    private suspend inline fun <reified T> get(
        route: String,
        query: Map<String, String?> = emptyMap(),
    ): T {
        val url = endpoint(route).newBuilder().apply {
            query.forEach { (name, value) -> value?.let { addQueryParameter(name, it) } }
        }.build()
        return execute(Request.Builder().url(url).get())
    }

    private suspend inline fun <reified T> post(
        route: String,
        jsonBody: String,
        headers: Map<String, String> = emptyMap(),
    ): T = execute(
        Request.Builder()
            .url(endpoint(route))
            .apply { headers.forEach { (name, value) -> header(name, value) } }
            .post(jsonBody.toRequestBody(JSON_MEDIA_TYPE)),
    )

    private suspend inline fun <reified T> put(route: String, jsonBody: String): T = execute(
        Request.Builder()
            .url(endpoint(route))
            .put(jsonBody.toRequestBody(JSON_MEDIA_TYPE)),
    )

    private suspend fun postExact(
        route: String,
        body: ByteArray,
        headers: Map<String, String> = emptyMap(),
    ): ByteArray = executeRaw(
        Request.Builder()
            .url(endpoint(route))
            .apply { headers.forEach { (name, value) -> header(name, value) } }
            .post(body.toRequestBody(JSON_MEDIA_TYPE)),
    )

    private suspend inline fun <reified T> execute(builder: Request.Builder): T {
        val token = tokens.accessToken()
        if (token.isNullOrBlank()) throw TransportException("sign_in_required")
        val request = builder.header("Authorization", "Bearer $token").build()
        return withContext(Dispatchers.IO) {
            val response = try {
                client.newCall(request).execute()
            } catch (failure: IOException) {
                throw TransportException("backend_unavailable", failure)
            }
            response.use {
                val body = boundedBody(it)
                if (it.code == 401) throw TransportException("sign_in_required")
                if (!it.isSuccessful) {
                    val code = runCatching {
                        json.decodeFromString(ApiErrorEnvelope.serializer(), body).error?.code
                    }.getOrNull() ?: "http_${it.code}"
                    throw TransportException(code)
                }
                runCatching { json.decodeFromString<T>(body) }
                    .getOrElse { failure -> throw TransportException("malformed_response", failure) }
            }
        }
    }

    private suspend fun executeRaw(builder: Request.Builder): ByteArray {
        val token = tokens.accessToken()
        if (token.isNullOrBlank()) throw TransportException("sign_in_required")
        val request = builder.header("Authorization", "Bearer $token").build()
        return withContext(Dispatchers.IO) {
            val response = try {
                client.newCall(request).execute()
            } catch (failure: IOException) {
                throw TransportException("backend_unavailable", failure)
            }
            response.use {
                val body = boundedBodyBytes(it)
                if (it.code == 401) throw TransportException("sign_in_required")
                if (!it.isSuccessful) {
                    val code = runCatching {
                        json.decodeFromString(
                            ApiErrorEnvelope.serializer(),
                            body.toString(Charsets.UTF_8),
                        ).error?.code
                    }.getOrNull() ?: "http_${it.code}"
                    throw TransportException(code)
                }
                body
            }
        }
    }

    private fun boundedBody(response: Response): String {
        return boundedBodyBytes(response).toString(Charsets.UTF_8)
    }

    private fun boundedBodyBytes(response: Response): ByteArray {
        val body = response.body ?: throw TransportException("empty_response")
        if (body.contentLength() > MAX_RESPONSE_BYTES) throw TransportException("response_too_large")
        val source = body.source()
        source.request(MAX_RESPONSE_BYTES + 1L)
        if (source.buffer.size > MAX_RESPONSE_BYTES) throw TransportException("response_too_large")
        return source.readByteArray()
    }

    private fun mobileRelayRoute(
        template: String,
        installationId: String,
        enrollmentId: String? = null,
    ): String {
        if (!isCanonicalLowercaseUuid(installationId) ||
            enrollmentId?.let { !isCanonicalLowercaseUuid(it) } == true
        ) throw TransportException("invalid_mobile_relay_identity")
        var route = template.replace("{installation_id}", installationId)
        if (enrollmentId != null) route = route.replace("{enrollment_id}", enrollmentId)
        if (route.contains('{') || route.contains('}')) throw TransportException("invalid_mobile_relay_route")
        return route
    }

    private fun endpoint(route: String): HttpUrl {
        if (!route.startsWith('/') || route.startsWith("//")) {
            throw IllegalArgumentException("invalid_backend_route")
        }
        val resolved = configuration.parsedBaseUrl.resolve(route)
            ?: throw IllegalArgumentException("invalid_backend_route")
        require(
            resolved.scheme == configuration.parsedBaseUrl.scheme &&
                resolved.host == configuration.parsedBaseUrl.host &&
                resolved.port == configuration.parsedBaseUrl.port,
        ) { "invalid_backend_route" }
        return resolved
    }

    companion object {
        private val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()
        private const val MAX_RESPONSE_BYTES = 1024 * 1024L
        private const val EVENT_POLL_MILLIS = 2_000L
    }
}

private fun isUuid(value: String): Boolean = runCatching { UUID.fromString(value) }.isSuccess

private fun isCanonicalLowercaseUuid(value: String): Boolean = runCatching {
    value == value.lowercase() && UUID.fromString(value).toString() == value &&
        value != "00000000-0000-0000-0000-000000000000"
}.getOrDefault(false)
