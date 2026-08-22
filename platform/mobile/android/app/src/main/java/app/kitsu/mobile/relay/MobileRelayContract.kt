package app.kitsu.mobile.relay

import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.OwnerEnrollmentChallenge
import app.kitsu.mobile.security.BondedCompanion
import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.TransportException
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID
import kotlinx.coroutines.flow.Flow
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

const val MAX_MOBILE_RELAY_DEVICES = 3
const val MOBILE_RELAY_CHUNK_BYTES = 8_192
const val MAX_RELAY_ENROLLMENT_REQUEST_BYTES = 12_000
const val MAX_RELAY_ENROLLMENT_RESPONSE_BYTES = 32_768
const val MAX_RELAY_FRAME_BYTES = 16_384

data class MobileRelayHttpRoutes(
    val binding: String = "/v1/mobile-relays/{installation_id}",
    val claim: String = "/v1/mobile-relays/{installation_id}/enrollments/{enrollment_id}/claim",
    val envelopes: String = "/v1/mobile-relays/{installation_id}/envelopes",
    val session: String = "/v1/mobile-relays/{installation_id}/session",
)

data class DeviceRelayHttpRoutes(
    val binding: String = "/v1/device-relays/{installation_id}",
    val enrollments: String = "/v1/device-relays/{installation_id}/enrollments",
    val claim: String = "/v1/device-relays/{installation_id}/enrollments/{enrollment_id}/claim",
    val envelopes: String = "/v1/device-relays/{installation_id}/envelopes",
    val session: String = "/v1/device-relays/{installation_id}/session",
)

object DeviceRelayAuthorization {
    const val SCHEME = "KitsuRelay"

    fun headerValue(relayCredentialB64: String): String {
        if (!MobileRelayWirePolicy.canonicalRelayCredential(relayCredentialB64)) {
            throw TransportException("relay_credential_unavailable")
        }
        return "$SCHEME $relayCredentialB64"
    }
}

data class MobileRelayBleOperations(
    val exchange: String = "mobile.relay.exchange",
)

object MobileRelayBondPolicy {
    fun upsert(existing: List<BondedCompanion>, value: BondedCompanion): List<BondedCompanion> =
        existing.filterNot {
            it.deviceAddress.equals(value.deviceAddress, ignoreCase = true) ||
                it.controllerIdB64 == value.controllerIdB64
        }.plus(value).takeLast(MAX_MOBILE_RELAY_DEVICES)
}

@Serializable
data class MobileRelayIdentity(
    @SerialName("installation_id") val installationId: String,
    @SerialName("gateway_id") val gatewayId: String,
    @SerialName("created_at") val createdAt: String,
    @SerialName("ca_cert_der_b64") val caCertificateDerB64: String,
)

@Serializable
data class MobileRelayBindingRequest(
    @SerialName("gateway_id") val gatewayId: String,
)

/** Small encrypted preference only; frames and enrollment material are never persisted here. */
@Serializable
data class MobileRelaySettings(
    @SerialName("installation_id") val installationId: String,
    @SerialName("relay_credential_b64") val relayCredentialB64: String? = null,
    val enabled: Boolean = false,
    @SerialName("activation_attempted") val activationAttempted: Boolean = false,
    @SerialName("activation_complete") val activationComplete: Boolean = false,
    @SerialName("forget_pending") val forgetPending: Boolean = false,
    @SerialName("selected_device_addresses") val selectedDeviceAddresses: List<String> = emptyList(),
    @SerialName("configured_device_addresses") val configuredDeviceAddresses: List<String> = emptyList(),
    @SerialName("companion_bindings") val companionBindings: List<MobileRelayCompanionBinding> = emptyList(),
    @SerialName("pending_enrollment") val pendingEnrollment: MobileRelayPendingEnrollment? = null,
)

@Serializable
data class MobileRelayCompanionBinding(
    @SerialName("hardware_uid") val hardwareUid: String,
    @SerialName("companion_id") val companionId: String,
    @SerialName("device_address") val deviceAddress: String? = null,
)

@Serializable
data class GatewayForgetReceipt(
    val schema: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

data class MobileRelayBackendConnectionState(
    val connected: Boolean = false,
    val detail: String = "off",
)

/** Bounded crash-recovery metadata; claim tokens and issuer documents remain memory-only. */
@Serializable
data class MobileRelayPendingEnrollment(
    @SerialName("hardware_uid") val hardwareUid: String,
    @SerialName("enrollment_id") val enrollmentId: String,
    @SerialName("expires_at") val expiresAt: String,
)

object MobileRelaySettingsPolicy {
    fun canStartAutomatically(settings: MobileRelaySettings): Boolean =
        settings.enabled && settings.activationComplete && !settings.forgetPending

    fun migrateLegacy(
        existing: MobileRelaySettings?,
        installationId: String,
        relayCredentialB64: String,
    ): MobileRelaySettings {
        require(MobileRelayWirePolicy.canonicalUuid(installationId))
        require(MobileRelayWirePolicy.canonicalRelayCredential(relayCredentialB64))
        return MobileRelaySettings(
            installationId = installationId,
            relayCredentialB64 = relayCredentialB64,
            enabled = existing?.enabled ?: false,
            selectedDeviceAddresses = existing?.selectedDeviceAddresses ?: emptyList(),
        )
    }

    fun bindCompanion(
        existing: List<MobileRelayCompanionBinding>,
        binding: MobileRelayCompanionBinding,
    ): List<MobileRelayCompanionBinding> = existing.filterNot {
        it.hardwareUid == binding.hardwareUid || it.companionId == binding.companionId
    }.plus(binding).takeLast(MAX_MOBILE_RELAY_DEVICES)
}

@Serializable
data class MobileRelayClaimResponse(
    @SerialName("companion_id") val companionId: String,
    @SerialName("gateway_id") val gatewayId: String,
    @SerialName("key_version") val keyVersion: Int,
)

@Serializable
data class MobileRelayEnvelopeAccepted(
    val accepted: Boolean,
    @SerialName("spool_record_id") val spoolRecordId: String,
    val sequence: String,
)

@Serializable
data class MobileRelayGatewayAck(
    val v: Int,
    val type: String,
    @SerialName("spool_record_id") val spoolRecordId: String,
    @SerialName("device_sequence") val deviceSequence: String,
)

@Serializable
data class MobileRelayRemoteActionHeader(
    val schema: String,
    @SerialName("action_id") val actionId: String,
    @SerialName("companion_id") val companionId: String,
)

@Serializable
data class MobileRelayChunk(
    val schema: String,
    val kind: String,
    val available: Boolean,
    val offset: Int,
    val total: Int,
    @SerialName("data_b64") val dataB64: String,
    val final: Boolean,
)

@Serializable
data class MobileRelayReceipt(
    val schema: String,
    val kind: String,
    val accepted: Boolean,
    @SerialName("next_offset") val nextOffset: Int,
    val complete: Boolean,
    @SerialName("error_code") val errorCode: String? = null,
)

enum class MobileRelayPullKind(val wireName: String, val responseKind: String, val maxBytes: Int) {
    ENROLLMENT("enrollment_pull", "enrollment", MAX_RELAY_ENROLLMENT_REQUEST_BYTES),
    UPLINK("uplink_pull", "uplink", MAX_RELAY_FRAME_BYTES),
}

enum class MobileRelayPushKind(val wireName: String, val responseKind: String, val maxBytes: Int) {
    ENROLLMENT("enrollment_push", "enrollment", MAX_RELAY_ENROLLMENT_RESPONSE_BYTES),
    DOWNLINK("downlink_push", "downlink", MAX_RELAY_FRAME_BYTES),
}

interface MobileRelayBackend {
    suspend fun ensureRelay(installationId: String, gatewayId: String): MobileRelayIdentity
    suspend fun createEnrollment(
        installationId: String,
        hardwareUid: String,
        displayName: String,
    ): OwnerEnrollmentChallenge
    suspend fun claimEnrollment(installationId: String, enrollmentId: String, exactRequest: ByteArray): ByteArray
    suspend fun uploadEnvelope(
        installationId: String,
        spoolRecordId: String,
        exactEnvelope: ByteArray,
    ): ByteArray
    fun downlinks(
        installationId: String,
        onConnectionState: (MobileRelayBackendConnectionState) -> Unit,
    ): Flow<ByteArray>
    suspend fun forgetRelay(installationId: String)
}

interface MobileRelayDeviceSession {
    suspend fun connect(): ConnectResult
    fun isConnected(): Boolean = false
    suspend fun disconnect()
    suspend fun status(): KitsuStatus
    suspend fun beginEnrollment(enrollmentId: String, claimToken: String): GatewayEnrollmentReceipt
    suspend fun finishEnrollment(enrollmentId: String): GatewayEnrollmentReceipt
    suspend fun configureRelay(gatewayId: String, caCertificateDerB64: String): MobileRelayReceipt
    suspend fun forgetGateway(expectedGatewayId: String): GatewayForgetReceipt
    suspend fun pull(kind: MobileRelayPullKind, offset: Int): MobileRelayChunk
    suspend fun push(
        kind: MobileRelayPushKind,
        offset: Int,
        total: Int,
        data: ByteArray,
        final: Boolean,
    ): MobileRelayReceipt
}

fun interface MobileRelayDeviceSessionFactory {
    fun create(companion: BondedCompanion): MobileRelayDeviceSession
}

object MobileRelayWirePolicy {
    const val EXCHANGE_SCHEMA = "kitsu.mobile-relay.exchange.v1"
    const val CHUNK_SCHEMA = "kitsu.mobile-relay.chunk.v1"
    const val RECEIPT_SCHEMA = "kitsu.mobile-relay.receipt.v1"
    const val REMOTE_ACTION_SCHEMA = "kitsu.remote-action.v1"
    const val RELAY_CREDENTIAL_BYTES = 32

    fun canonicalUuid(value: String): Boolean = runCatching {
        value == value.lowercase() && UUID.fromString(value).toString() == value &&
            value != "00000000-0000-0000-0000-000000000000"
    }.getOrDefault(false)

    fun canonicalU64(value: String): Boolean {
        if (!Regex("^(0|[1-9][0-9]{0,18})$").matches(value)) return false
        return value.toULongOrNull() != null && value.toULong() <= Long.MAX_VALUE.toULong()
    }

    fun canonicalRelayCredential(value: String?): Boolean {
        if (value == null || value.length != 43) return false
        return runCatching {
            val decoded = decodeCanonical(value, RELAY_CREDENTIAL_BYTES)
            decoded.size == RELAY_CREDENTIAL_BYTES
        }.getOrDefault(false)
    }

    fun decodeCanonical(value: String, maxBytes: Int): ByteArray {
        val decoded = runCatching { Base64.getUrlDecoder().decode(value) }
            .getOrElse { throw TransportException("malformed_relay_base64", it) }
        if (decoded.size > maxBytes || Base64.getUrlEncoder().withoutPadding().encodeToString(decoded) != value) {
            throw TransportException("malformed_relay_base64")
        }
        return decoded
    }

    /** Stable logical gateway ID derived from the installation UUID, without another stored secret. */
    fun gatewayId(installationId: String): String {
        if (!canonicalUuid(installationId)) throw TransportException("invalid_mobile_relay_identity")
        val source = UUID.fromString(installationId)
        val input = ByteBuffer.allocate(16).run {
            putLong(source.mostSignificantBits)
            putLong(source.leastSignificantBits)
            array()
        }
        val bytes = MessageDigest.getInstance("SHA-256")
            .digest("kitsu.mobile-relay.gateway.v1\u0000".toByteArray() + input)
            .copyOf(16)
        bytes[6] = ((bytes[6].toInt() and 0x0f) or 0x50).toByte()
        bytes[8] = ((bytes[8].toInt() and 0x3f) or 0x80).toByte()
        val value = ByteBuffer.wrap(bytes)
        return UUID(value.long, value.long).toString()
    }

    fun gatewayAcknowledgement(spoolRecordId: String, deviceSequence: String): ByteArray {
        if (!canonicalU64(spoolRecordId) || spoolRecordId == "0" ||
            !canonicalU64(deviceSequence) || deviceSequence == "0"
        ) throw TransportException("malformed_envelope_ack")
        return Json.encodeToString(
            MobileRelayGatewayAck.serializer(),
            MobileRelayGatewayAck(
                v = 1,
                type = "gateway_ack",
                spoolRecordId = spoolRecordId,
                deviceSequence = deviceSequence,
            ),
        ).toByteArray(Charsets.UTF_8)
    }
}

object MobileRelayTransfer {
    suspend fun pull(
        kind: MobileRelayPullKind,
        exchange: suspend (MobileRelayPullKind, Int) -> MobileRelayChunk,
    ): ByteArray? {
        var offset = 0
        var expectedTotal: Int? = null
        val output = ByteArrayOutputStream()
        while (true) {
            val chunk = exchange(kind, offset)
            if (chunk.schema != MobileRelayWirePolicy.CHUNK_SCHEMA || chunk.kind != kind.responseKind) {
                throw TransportException("malformed_relay_chunk")
            }
            if (!chunk.available) {
                if (offset != 0 || chunk.offset != 0 || chunk.total != 0 || chunk.dataB64.isNotEmpty() || chunk.final) {
                    throw TransportException("malformed_relay_chunk")
                }
                return null
            }
            if (chunk.offset != offset || chunk.total !in 1..kind.maxBytes ||
                expectedTotal?.let { it != chunk.total } == true
            ) throw TransportException("malformed_relay_chunk")
            expectedTotal = chunk.total
            val remaining = chunk.total - offset
            val decoded = MobileRelayWirePolicy.decodeCanonical(
                chunk.dataB64,
                minOf(MOBILE_RELAY_CHUNK_BYTES, remaining),
            )
            if (decoded.isEmpty() || decoded.size > remaining) throw TransportException("malformed_relay_chunk")
            output.write(decoded)
            offset += decoded.size
            val atEnd = offset == chunk.total
            if (chunk.final != atEnd) throw TransportException("malformed_relay_chunk")
            if (atEnd) return output.toByteArray()
        }
    }

    suspend fun push(
        kind: MobileRelayPushKind,
        payload: ByteArray,
        exchange: suspend (MobileRelayPushKind, Int, Int, ByteArray, Boolean) -> MobileRelayReceipt,
    ) {
        if (payload.isEmpty() || payload.size > kind.maxBytes) throw TransportException("relay_payload_size")
        var offset = 0
        while (offset < payload.size) {
            val end = minOf(offset + MOBILE_RELAY_CHUNK_BYTES, payload.size)
            val final = end == payload.size
            val receipt = exchange(kind, offset, payload.size, payload.copyOfRange(offset, end), final)
            if (receipt.schema != MobileRelayWirePolicy.RECEIPT_SCHEMA ||
                receipt.kind != kind.responseKind || !receipt.accepted ||
                receipt.nextOffset != end || receipt.complete != final || receipt.errorCode != null
            ) throw TransportException(receipt.errorCode ?: "relay_push_rejected")
            offset = end
        }
    }
}
