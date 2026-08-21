package app.kitsu.mobile.transport

import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.GatewayEnrollmentEvent
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import app.kitsu.mobile.model.MeshState
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.MeshPeerKeyPolicy
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.NeedLevels
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiRetryReceipt
import app.kitsu.mobile.model.WIRE_VERSION
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** Maps the compact, authenticated Heltec operation payloads into app models. */
internal object FirmwareBlePayloadMapper {
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }

    @Serializable
    private data class FirmwareState(
        val schema: String,
        @SerialName("device_uid") val deviceUid: String,
        val companion: String,
        val energy: Int,
        val curiosity: Int,
        val affection: Int,
        val sleeping: Boolean,
        @SerialName("mesh_rx_ready") val meshRxReady: Boolean,
        @SerialName("mesh_tx_unlocked") val meshTxUnlocked: Boolean,
        @SerialName("mesh_enabled") val meshEnabled: Boolean = false,
        @SerialName("mesh_time_valid") val meshTimeValid: Boolean = false,
        @SerialName("mesh_one_shot_ready") val meshOneShotReady: Boolean = false,
        @SerialName("wifi_configured") val wifiConfigured: Boolean = false,
        @SerialName("wifi_state") val wifiState: String = "unconfigured",
        @SerialName("gateway_configured") val gatewayConfigured: Boolean = false,
        @SerialName("gateway_enrolled") val gatewayEnrolled: Boolean = false,
        @SerialName("lan_state") val lanState: String = "unconfigured",
        @SerialName("gateway_lan_state") val gatewayLanState: String? = null,
        @SerialName("gateway_enrollment_state") val gatewayEnrollmentState: String = "idle",
        @SerialName("gateway_enrollment_error") val gatewayEnrollmentError: String? = null,
        @SerialName("gateway_enrollment_expires_in_ms") val gatewayEnrollmentExpiresInMs: Int = 0,
        @SerialName("remote_connectivity_allowed") val remoteConnectivityAllowed: Boolean,
        @SerialName("event_count") val eventCount: Int = 0,
    )

    @Serializable
    private data class Observation(
        @SerialName("epoch_valid") val epochValid: Boolean,
        val epoch: Long,
    )

    @Serializable
    private data class LastHop(
        val valid: Boolean,
        val rssi: Double,
        val snr: Double,
    )

    @Serializable
    private data class FirmwareHistoryItem(
        val sequence: String,
        @SerialName("public_key_b64") val publicKeyB64: String,
        val observed: Observation,
        @SerialName("last_hop") val lastHop: LastHop,
    )

    @Serializable
    private data class FirmwareHistoryPage(
        val schema: String,
        val items: List<FirmwareHistoryItem>,
        val cursor: String? = null,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable
    private data class FirmwarePeer(
        @SerialName("public_key_b64") val publicKeyB64: String,
        val name: String,
        val type: Int,
        @SerialName("last_observed") val lastObserved: Observation,
        @SerialName("last_hop") val lastHop: LastHop,
    )

    @Serializable
    private data class FirmwarePeerPage(
        val schema: String,
        val items: List<FirmwarePeer>,
    )

    @Serializable
    private data class FirmwareMessage(
        @SerialName("message_id") val messageId: String,
        val timestamp: Long,
        val inbound: Boolean,
        val kind: String,
        val authenticated: Boolean,
        @SerialName("sender_name") val senderName: String,
        @SerialName("peer_id") val peerId: String? = null,
        @SerialName("channel_slot") val channelSlot: Int? = null,
        val text: String,
        val state: String,
    )

    @Serializable
    private data class FirmwareMessagePage(
        val schema: String,
        val items: List<FirmwareMessage>,
        val cursor: String? = null,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable
    private data class FirmwareChannel(
        val slot: Int,
        val configured: Boolean,
        val name: String? = null,
    )

    @Serializable
    private data class FirmwareChannelPage(
        val schema: String,
        val items: List<FirmwareChannel>,
    )

    @Serializable
    private data class FirmwareMeshConfiguration(
        val schema: String,
        val enabled: Boolean,
        val profile: String,
        @SerialName("tx_power_dbm") val txPowerDbm: Int,
    )

    @Serializable
    private data class FirmwareProvisioningReceipt(
        val schema: String,
        val accepted: Boolean,
        val state: String,
        @SerialName("error_code") val errorCode: String? = null,
    )

    @Serializable
    private data class FirmwareGatewayEnrollmentReceipt(
        val schema: String,
        val accepted: Boolean,
        val state: String,
        @SerialName("enrollment_id") val enrollmentId: String,
        @SerialName("expires_in_ms") val expiresInMs: Int? = null,
        @SerialName("error_code") val errorCode: String? = null,
    )

    fun rejectionCode(payload: ByteArray): String? = try {
        val root = json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
        if (root["ok"]?.jsonPrimitive?.booleanOrNull == false) {
            root["error"]?.jsonPrimitive?.content ?: "request_rejected"
        } else {
            null
        }
    } catch (_: Throwable) {
        null
    }

    fun state(payload: ByteArray, observedAtEpochSeconds: Long): KitsuStatus {
        val value = decode<FirmwareState>(payload, "malformed_state")
        requireSchema(value.schema, "kitsu.state.v1")
        if (value.energy !in 0..100 || value.curiosity !in 0..100 || value.affection !in 0..100) {
            throw TransportException("malformed_state")
        }
        val gatewayLanState = value.gatewayLanState ?: value.lanState
        if (value.wifiState !in WIFI_STATES || gatewayLanState !in LAN_STATES) {
            throw TransportException("malformed_state")
        }
        return KitsuStatus(
            protocol = WIRE_VERSION,
            deviceId = value.deviceUid,
            displayName = value.companion,
            companionName = value.companion,
            mood = if (value.sleeping) "SLEEPING" else "AWAKE",
            needs = NeedLevels(value.energy, value.curiosity, value.affection),
            mesh = MeshState(
                enabled = value.meshEnabled,
                rxReady = value.meshRxReady,
                txReady = value.meshOneShotReady,
                timeValid = value.meshTimeValid,
                oneShotReady = value.meshOneShotReady,
            ),
            lan = LanState(
                wifiConfigured = value.wifiConfigured,
                wifiState = value.wifiState,
                gatewayConfigured = value.gatewayConfigured,
                gatewayEnrolled = value.gatewayEnrolled,
                lanState = gatewayLanState,
                gatewayEnrollmentState = value.gatewayEnrollmentState,
                gatewayEnrollmentError = value.gatewayEnrollmentError,
                gatewayEnrollmentExpiresInMs = value.gatewayEnrollmentExpiresInMs,
                remoteConnectivityAllowed = value.remoteConnectivityAllowed,
            ),
            cursor = value.eventCount.takeIf { it > 0 }?.toString(),
            updatedAt = observedAtEpochSeconds,
        )
    }

    fun history(payload: ByteArray): HistoryPage {
        val value = decode<FirmwareHistoryPage>(payload, "malformed_history")
        requireSchema(value.schema, "kitsu.history.v1")
        if (value.items.any { !MeshPeerKeyPolicy.isCanonicalBase64Url(it.publicKeyB64) }) {
            throw TransportException("malformed_history")
        }
        return HistoryPage(
            items = value.items.map { item ->
                val signal = if (item.lastHop.valid) " · ${item.lastHop.rssi} dBm last hop" else ""
                HistoryEntry(
                    id = item.sequence,
                    cursor = item.sequence,
                    kind = "mesh advert",
                    summary = "${item.publicKeyB64.take(10)}$signal",
                    occurredAt = item.observed.epoch.takeIf { item.observed.epochValid } ?: 0,
                )
            },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
        )
    }

    fun peers(payload: ByteArray): PeerPage {
        val value = decode<FirmwarePeerPage>(payload, "malformed_peers")
        requireSchema(value.schema, "kitsu.peers.v1")
        if (value.items.any { !MeshPeerKeyPolicy.isCanonicalBase64Url(it.publicKeyB64) }) {
            throw TransportException("malformed_peers")
        }
        return PeerPage(
            items = value.items.map { item ->
                Peer(
                    id = item.publicKeyB64,
                    name = item.name,
                    role = roleName(item.type),
                    lastHeardAt = item.lastObserved.epoch.takeIf { item.lastObserved.epochValid },
                    route = if (item.lastHop.valid) {
                        "last hop ${item.lastHop.rssi} dBm / ${item.lastHop.snr} dB"
                    } else {
                        null
                    },
                )
            },
        )
    }

    fun messages(payload: ByteArray): MessagePage {
        val value = decode<FirmwareMessagePage>(payload, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v1")
        if (value.items.any { item ->
                item.kind !in setOf("direct", "channel") ||
                    (item.peerId != null && !MeshPeerKeyPolicy.isCanonicalBase64Url(item.peerId)) ||
                    (item.kind == "direct" && item.peerId == null) ||
                    (item.kind == "channel" && item.channelSlot !in 0..3)
            }
        ) {
            throw TransportException("malformed_messages")
        }
        return MessagePage(
            items = value.items.map { item ->
                val directPeer = item.peerId ?: item.senderName.ifBlank { "direct peer" }
                val channelSender = item.senderName.takeIf { it.isNotBlank() }
                    ?.let { "$it (unverified)" }
                Message(
                    id = item.messageId,
                    cursor = item.messageId,
                    direction = if (item.inbound) "inbound" else "outbound",
                    peerId = when {
                        item.kind == "direct" -> directPeer
                        item.inbound -> channelSender
                        else -> null
                    },
                    channel = if (item.kind == "channel") item.channelSlot?.toString() ?: "unknown" else null,
                    text = item.text,
                    state = item.state,
                    occurredAt = item.timestamp,
                )
            },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
        )
    }

    fun channels(payload: ByteArray): List<MeshChannel> {
        val value = decode<FirmwareChannelPage>(payload, "malformed_channels")
        requireSchema(value.schema, "kitsu.channels.v1")
        if (value.items.size != 4 || value.items.map { it.slot } != listOf(0, 1, 2, 3) ||
            value.items.any { channel -> !validChannelName(channel.configured, channel.name) }
        ) {
            throw TransportException("malformed_channels")
        }
        return value.items.map { MeshChannel(it.slot, it.configured, it.name) }
    }

    fun meshConfiguration(payload: ByteArray): MeshConfigurationReceipt {
        val value = decode<FirmwareMeshConfiguration>(payload, "malformed_mesh_configuration")
        requireSchema(value.schema, "kitsu.mesh-config.v1")
        if (value.profile != "uk_eu_narrow" || value.txPowerDbm != 22) {
            throw TransportException("unexpected_mesh_profile")
        }
        return MeshConfigurationReceipt(value.enabled, value.profile, value.txPowerDbm)
    }

    fun wifiConfiguration(payload: ByteArray): ProvisioningReceipt {
        val value = provisioningReceipt(payload, "kitsu.wifi-config.v1")
        return ProvisioningReceipt(value.accepted, value.state, value.errorCode)
    }

    fun wifiRetry(payload: ByteArray): WifiRetryReceipt {
        val value = decode<FirmwareProvisioningReceipt>(payload, "malformed_wifi_retry")
        requireSchema(value.schema, "kitsu.wifi-retry.v1")
        if (!value.accepted || value.state != "retrying" || value.errorCode != null) {
            throw TransportException(value.errorCode ?: "wifi_retry_rejected")
        }
        return WifiRetryReceipt(value.accepted, value.state, value.errorCode)
    }

    fun gatewayConfiguration(payload: ByteArray): GatewayConfigurationReceipt {
        val value = provisioningReceipt(payload, "kitsu.gateway-config.v2")
        return GatewayConfigurationReceipt(value.accepted, value.state, value.errorCode)
    }

    fun gatewayEnrollmentBegin(payload: ByteArray, expectedEnrollmentId: String): GatewayEnrollmentReceipt {
        val value = gatewayEnrollmentReceipt(payload, expectedEnrollmentId)
        val expiresInMs = value.expiresInMs
        if (!value.accepted || value.state != "physical_confirmation_required" ||
            expiresInMs == null || expiresInMs !in 1..60_000 || value.errorCode != null
        ) {
            throw TransportException(value.errorCode ?: "malformed_enrollment_receipt")
        }
        return value
    }

    fun gatewayEnrollmentFinish(payload: ByteArray, expectedEnrollmentId: String): GatewayEnrollmentReceipt {
        val value = gatewayEnrollmentReceipt(payload, expectedEnrollmentId)
        val waiting = !value.accepted && value.state == "physical_confirmation_required" &&
            value.errorCode == "physical_confirmation_required" &&
            value.expiresInMs != null && value.expiresInMs in 1..60_000
        val ready = value.accepted && value.state == "ready_for_wifi" &&
            value.errorCode == null && value.expiresInMs == 300_000
        if (!waiting && !ready) {
            throw TransportException(value.errorCode ?: "malformed_enrollment_receipt")
        }
        return value
    }

    fun gatewayEnrollmentEvent(payload: ByteArray): GatewayEnrollmentEvent {
        val value = decode<GatewayEnrollmentEvent>(payload, "malformed_enrollment_event")
        val physicalConfirmed = value.accepted && value.state == "physical_confirmed" &&
            value.enrollmentId?.let(EnrollmentPolicy::canonicalUuid) == true &&
            value.expiresInMs in 1..60_000 && value.errorCode == null
        val expired = !value.accepted && value.state == "expired" &&
            value.enrollmentId == null && value.expiresInMs == 0 && value.errorCode == "expired"
        if (value.schema != "kitsu.gateway-enrollment.event.v1" ||
            (!physicalConfirmed && !expired)
        ) {
            throw TransportException("malformed_enrollment_event")
        }
        return value
    }

    private fun gatewayEnrollmentReceipt(
        payload: ByteArray,
        expectedEnrollmentId: String,
    ): GatewayEnrollmentReceipt {
        val value = decode<FirmwareGatewayEnrollmentReceipt>(payload, "malformed_enrollment_receipt")
        requireSchema(value.schema, "kitsu.gateway-enrollment.receipt.v1")
        if (!EnrollmentPolicy.canonicalUuid(value.enrollmentId) || value.enrollmentId != expectedEnrollmentId) {
            throw TransportException("enrollment_response_binding_failed")
        }
        return GatewayEnrollmentReceipt(
            schema = value.schema,
            accepted = value.accepted,
            state = value.state,
            enrollmentId = value.enrollmentId,
            expiresInMs = value.expiresInMs,
            errorCode = value.errorCode,
        )
    }

    private fun provisioningReceipt(payload: ByteArray, schema: String): FirmwareProvisioningReceipt {
        val value = decode<FirmwareProvisioningReceipt>(payload, "malformed_provisioning_receipt")
        requireSchema(value.schema, schema)
        if (!value.accepted || value.state != "stored" || value.errorCode != null) {
            throw TransportException(value.errorCode ?: "provisioning_rejected")
        }
        return value
    }

    private inline fun <reified T> decode(payload: ByteArray, code: String): T = try {
        json.decodeFromString(payload.toString(Charsets.UTF_8))
    } catch (failure: Throwable) {
        throw TransportException(code, failure)
    }

    private fun requireSchema(actual: String, expected: String) {
        if (actual != expected) throw TransportException("unsupported_protocol")
    }

    private fun validChannelName(configured: Boolean, name: String?): Boolean = when {
        !configured -> name == null
        name == null -> false
        name.toByteArray(Charsets.UTF_8).size !in 1..32 -> false
        name.any { it.isISOControl() || Character.isSurrogate(it) } -> false
        else -> true
    }

    private fun roleName(type: Int): String = when (type) {
        1 -> "client"
        2 -> "repeater"
        3 -> "room"
        4 -> "sensor"
        else -> "unknown"
    }

    private val WIFI_STATES = setOf(
        "unconfigured",
        "storage_unavailable",
        "connectivity_unavailable",
        "ble_active",
        "grace",
        "connecting",
        "connected",
        "backoff",
    )
    private val LAN_STATES = setOf(
        "config_unavailable",
        "connectivity_unavailable",
        "unconfigured",
        "ble_priority",
        "wifi_pending",
        "enrollment_pending",
        "time_pending",
        "replay_unavailable",
        "reconnecting",
        "connected",
    )
}
