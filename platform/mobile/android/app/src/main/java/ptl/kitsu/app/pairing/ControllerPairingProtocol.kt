package ptl.kitsu.app.pairing

import ptl.kitsu.app.security.ControllerRole
import ptl.kitsu.app.transport.SecureEnvelopeSession
import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

interface PairingChannel {
    suspend fun send(payload: ByteArray)
    suspend fun receive(): ByteArray
}

@Serializable
private data class PairRequest(
    val v: Int = 1,
    val type: String = "pair_request",
    @SerialName("client_nonce_b64") val clientNonceB64: String,
    val label: String,
    val platform: String = "android",
)

@Serializable
private data class PairPending(
    val v: Int,
    val type: String,
    val role: String? = null,
    @SerialName("device_nonce_b64") val deviceNonceB64: String,
    @SerialName("expires_in_ms") val expiresInMs: Int,
)

@Serializable
private data class PairGrant(
    val v: Int,
    val type: String,
    val role: String? = null,
    @SerialName("controller_id_b64") val controllerIdB64: String,
    @SerialName("root_b64") val rootB64: String,
    @SerialName("device_uid") val deviceUid: String,
    @SerialName("client_nonce_b64") val clientNonceB64: String,
    @SerialName("device_nonce_b64") val deviceNonceB64: String,
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class PairCommitV1(
    val v: Int = 1,
    val type: String = "pair_commit",
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class PairCommitV2(
    val v: Int = 2,
    val type: String = "pair_commit",
    val role: String = "caretaker",
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class PairOk(
    val v: Int,
    val type: String,
    val role: String? = null,
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class PairError(
    val v: Int,
    val type: String,
    val code: String,
)

data class ControllerGrant(
    val controllerIdB64: String,
    val rootB64: String,
    val deviceUid: String,
    val role: ControllerRole = ControllerRole.OWNER,
)

enum class ControllerPairingFlow {
    OWNER,
    CARETAKER,
}

class ControllerPairingProtocol(private val random: SecureRandom = SecureRandom()) {
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = false
        // PairRequest and PairCommit use defaulted protocol constants; encode
        // them explicitly because the firmware's pre-session parser is strict.
        encodeDefaults = true
    }

    suspend fun pair(
        label: String,
        channel: PairingChannel,
        persistCandidate: suspend (ControllerGrant) -> Unit,
        deleteCandidate: suspend () -> Unit,
        onPending: suspend (expiresInMillis: Int) -> Unit = {},
        onCandidateStored: suspend () -> Unit = {},
        flow: ControllerPairingFlow = ControllerPairingFlow.OWNER,
    ): ControllerGrant {
        val cleanLabel = sanitizedLabel(label)
        val clientNonce = ByteArray(16).also(random::nextBytes)
        channel.send(
            bounded(
                json.encodeToString(
                    PairRequest(
                        v = flow.protocolVersion,
                        clientNonceB64 = SecureEnvelopeSession.encodeUrl(clientNonce),
                        label = cleanLabel,
                    ),
                ).toByteArray(Charsets.UTF_8),
            ),
        )
        val pending = try {
            withTimeout(RESPONSE_TIMEOUT_MILLIS) {
                decode<PairPending>(bounded(channel.receive()))
            }
        } catch (failure: TimeoutCancellationException) {
            throw PairingException("pairing_timeout", failure)
        }
        if (!flow.accepts(pending.v, pending.role) ||
            pending.type != "pair_pending" || pending.expiresInMs !in 1..60_000
        ) {
            throw PairingException("pairing_failed")
        }
        val deviceNonce = decodeFixed(pending.deviceNonceB64, 16)
        onPending(pending.expiresInMs)
        val grant = try {
            withTimeout(pending.expiresInMs.toLong()) {
                decode<PairGrant>(bounded(channel.receive()))
            }
        } catch (failure: TimeoutCancellationException) {
            throw PairingException("pairing_timeout", failure)
        }
        if (!flow.accepts(grant.v, grant.role) || grant.type != "pair_grant" ||
            grant.clientNonceB64 != SecureEnvelopeSession.encodeUrl(clientNonce) ||
            grant.deviceNonceB64 != SecureEnvelopeSession.encodeUrl(deviceNonce)
        ) throw PairingException("pairing_failed")

        val controllerId = decodeFixed(grant.controllerIdB64, 16)
        val root = decodeFixed(grant.rootB64, 32)
        if (!DEVICE_UID.matches(grant.deviceUid)) {
            root.fill(0)
            throw PairingException("pairing_failed")
        }
        val expectedDevice = pairingProof(
            flow, root, DEVICE_ROLE, controllerId, grant.deviceUid, clientNonce, deviceNonce,
        )
        val actualDevice = decodeFixed(grant.proofB64, 32)
        if (!MessageDigest.isEqual(expectedDevice, actualDevice)) {
            root.fill(0)
            throw PairingException("pairing_failed")
        }

        val candidate = ControllerGrant(
            grant.controllerIdB64,
            grant.rootB64,
            grant.deviceUid,
            flow.attestedRole,
        )
        var candidateStored = false
        try {
            persistCandidate(candidate)
            candidateStored = true
            onCandidateStored()
            val clientProof = pairingProof(
                flow, root, CLIENT_ROLE, controllerId, grant.deviceUid, clientNonce, deviceNonce,
            )
            val commit = when (flow) {
                ControllerPairingFlow.OWNER -> json.encodeToString(
                    PairCommitV1(proofB64 = SecureEnvelopeSession.encodeUrl(clientProof)),
                )
                ControllerPairingFlow.CARETAKER -> json.encodeToString(
                    PairCommitV2(proofB64 = SecureEnvelopeSession.encodeUrl(clientProof)),
                )
            }
            channel.send(
                bounded(commit.toByteArray(Charsets.UTF_8)),
            )
            val ok = try {
                withTimeout(RESPONSE_TIMEOUT_MILLIS) {
                    decode<PairOk>(bounded(channel.receive()))
                }
            } catch (failure: TimeoutCancellationException) {
                throw PairingException("pairing_timeout", failure)
            }
            val expectedOk = pairingProof(
                flow, root, OK_ROLE, controllerId, grant.deviceUid, clientNonce, deviceNonce,
            )
            val actualOk = decodeFixed(ok.proofB64, 32)
            if (!flow.accepts(ok.v, ok.role) || ok.type != "pair_ok" ||
                !MessageDigest.isEqual(expectedOk, actualOk)
            ) {
                throw PairingException("pairing_failed")
            }
            root.fill(0)
            return candidate
        } catch (failure: Throwable) {
            root.fill(0)
            // Once pair_commit may have reached Kitsu, a missing or malformed pair_ok is
            // not proof that the durable controller was rejected. Retain the candidate so
            // a normal authenticated handshake can finish recovery after process/GATT loss.
            val authoritativeRejection = (failure as? PairingException)?.code
                ?.let(AUTHORITATIVE_REJECTIONS::contains) == true
            if (!candidateStored || authoritativeRejection) {
                withContext(NonCancellable) { deleteCandidate() }
            }
            throw failure
        }
    }

    private inline fun <reified T> decode(bytes: ByteArray): T = try {
        val text = bytes.toString(Charsets.UTF_8)
        val type = json.parseToJsonElement(text).jsonObject["type"]?.jsonPrimitive?.content
        if (type == "error") {
            val error = json.decodeFromString<PairError>(text)
            if (error.v != 1 || error.type != "error" || error.code !in PAIRING_ERROR_CODES) {
                throw PairingException("pairing_failed")
            }
            throw PairingException(error.code)
        }
        json.decodeFromString(text)
    } catch (failure: PairingException) {
        throw failure
    } catch (failure: Throwable) {
        throw PairingException("pairing_failed", failure)
    }

    private fun bounded(value: ByteArray): ByteArray {
        if (value.isEmpty() || value.size > MAX_FRAME_BYTES) throw PairingException("pairing_failed")
        return value
    }

    private fun decodeFixed(value: String, size: Int): ByteArray =
        SecureEnvelopeSession.decodeUrl(value, "pairing_failed").also {
            if (it.size != size) throw PairingException("pairing_failed")
        }

    private fun sanitizedLabel(value: String): String {
        val clean = value.trim()
        val validControls = clean.codePoints().allMatch { Character.getType(it) != Character.CONTROL.toInt() }
        if (clean.isEmpty() || clean.toByteArray(Charsets.UTF_8).size > 24 || !validControls) {
            throw PairingException("invalid_label")
        }
        return clean
    }

    companion object {
        private val DEVICE_ROLE = "device".toByteArray(Charsets.US_ASCII)
        private val CLIENT_ROLE = "client".toByteArray(Charsets.US_ASCII)
        private val OK_ROLE = "ok".toByteArray(Charsets.US_ASCII)

        private fun pairingProof(
            flow: ControllerPairingFlow,
            root: ByteArray,
            proofRole: ByteArray,
            controllerId: ByteArray,
            deviceUid: String,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
        ): ByteArray = when (flow) {
            ControllerPairingFlow.OWNER -> proof(
                root,
                when {
                    proofRole.contentEquals(DEVICE_ROLE) -> DEVICE_PREFIX
                    proofRole.contentEquals(CLIENT_ROLE) -> CLIENT_PREFIX
                    else -> OK_PREFIX
                },
                controllerId,
                deviceUid,
                clientNonce,
                deviceNonce,
            )
            ControllerPairingFlow.CARETAKER -> roleBoundProof(
                root,
                proofRole,
                CARETAKER_ROLE,
                controllerId,
                deviceUid,
                clientNonce,
                deviceNonce,
            )
        }

        internal fun proof(
            root: ByteArray,
            prefix: ByteArray,
            controllerId: ByteArray,
            deviceUid: String,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
        ): ByteArray = SecureEnvelopeSession.hmac(
            root,
            prefix + controllerId + root + deviceUid.toByteArray(Charsets.US_ASCII) +
                clientNonce + deviceNonce,
        )

        internal fun roleBoundProof(
            root: ByteArray,
            proofRole: ByteArray,
            controllerRole: ByteArray,
            controllerId: ByteArray,
            deviceUid: String,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
        ): ByteArray = SecureEnvelopeSession.hmac(
            root,
            PAIR_V2_DOMAIN + proofRole + ZERO + controllerRole + ZERO +
                controllerId + root + deviceUid.toByteArray(Charsets.US_ASCII) +
                clientNonce + deviceNonce,
        )

        internal val DEVICE_PREFIX = "KITSU-PAIR-1\u0000device\u0000".toByteArray(Charsets.US_ASCII)
        internal val CLIENT_PREFIX = "KITSU-PAIR-1\u0000client\u0000".toByteArray(Charsets.US_ASCII)
        internal val OK_PREFIX = "KITSU-PAIR-1\u0000ok\u0000".toByteArray(Charsets.US_ASCII)
        internal val PAIR_V2_DOMAIN = "KITSU-PAIR-2\u0000".toByteArray(Charsets.US_ASCII)
        internal val CARETAKER_ROLE = "caretaker".toByteArray(Charsets.US_ASCII)
        private val ZERO = byteArrayOf(0)
        private val DEVICE_UID = Regex("^KT[0-9A-F]{4}$")
        private val PAIRING_ERROR_CODES = setOf(
            "pairing_closed", "controller_full", "auth_failed", "timeout",
        )
        private val AUTHORITATIVE_REJECTIONS = PAIRING_ERROR_CODES
        const val MAX_FRAME_BYTES = 1024
        private const val RESPONSE_TIMEOUT_MILLIS = 10_000L
    }
}

private val ControllerPairingFlow.protocolVersion: Int
    get() = when (this) {
        ControllerPairingFlow.OWNER -> 1
        ControllerPairingFlow.CARETAKER -> 2
    }

private val ControllerPairingFlow.attestedRole: ControllerRole
    get() = when (this) {
        ControllerPairingFlow.OWNER -> ControllerRole.OWNER
        ControllerPairingFlow.CARETAKER -> ControllerRole.CARETAKER
    }

private fun ControllerPairingFlow.accepts(version: Int, role: String?): Boolean = when (this) {
    ControllerPairingFlow.OWNER -> version == 1 && role == null
    ControllerPairingFlow.CARETAKER -> version == 2 && role == "caretaker"
}

class PairingException(val code: String, cause: Throwable? = null) : Exception(code, cause)
