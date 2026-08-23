package ptl.kitsu.app.transport

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

@Serializable
data class SecureOuterEnvelope(
    // Required on decode. The firmware rejects an omitted version field and
    // the version is deliberately not inferred from a Kotlin default.
    val v: Int,
    val channel: Int,
    val seq: String,
    @SerialName("nonce_b64") val nonceB64: String,
    @SerialName("request_id_b64") val requestIdB64: String,
    val op: String,
    @SerialName("payload_b64") val payloadB64: String,
    @SerialName("mac_b64") val macB64: String,
)

data class VerifiedEnvelope(
    val channel: Int,
    val sequence: ULong,
    val requestId: UUID,
    val operation: String,
    val payload: ByteArray,
)

class SecureEnvelopeSession private constructor(
    private val sendKey: ByteArray,
    private val receiveKey: ByteArray,
    private val random: SecureRandom,
    private val allowedIncomingChannels: Set<Int>,
) {
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = false
        // The firmware requires all eight outer fields, including v.
        encodeDefaults = true
    }
    private var nextSendSequence: ULong = 1u
    private var nextReceiveSequence: ULong = 1u

    @Synchronized
    fun encodeRequest(requestId: UUID, operation: String, payload: ByteArray): ByteArray {
        val sequence = nextSendSequence
        val encoded = encode(
            channel = CHANNEL_REQUEST,
            sequence = sequence,
            requestId = requestId,
            operation = operation,
            payload = payload,
            key = sendKey,
        )
        if (nextSendSequence == ULong.MAX_VALUE) throw EnvelopeException("sequence_exhausted")
        nextSendSequence++
        return encoded
    }

    @Synchronized
    internal fun encodeOutgoing(
        channel: Int,
        requestId: UUID,
        operation: String,
        payload: ByteArray,
    ): ByteArray {
        val sequence = nextSendSequence
        val encoded = encode(channel, sequence, requestId, operation, payload, sendKey)
        if (nextSendSequence == ULong.MAX_VALUE) throw EnvelopeException("sequence_exhausted")
        nextSendSequence++
        return encoded
    }

    @Synchronized
    fun decodeIncoming(encoded: ByteArray): VerifiedEnvelope {
        if (encoded.size > MAX_GATT_JSON_BYTES) throw EnvelopeException("frame_too_large")
        val encodedText = encoded.toString(Charsets.UTF_8)
        requireExactOuterKeys(encodedText)
        val outer = try {
            json.decodeFromString(SecureOuterEnvelope.serializer(), encodedText)
        } catch (failure: Throwable) {
            throw EnvelopeException("malformed_envelope", failure)
        }
        if (outer.v != 1) throw EnvelopeException("unsupported_envelope")
        if (outer.channel !in allowedIncomingChannels) {
            throw EnvelopeException("invalid_incoming_channel")
        }
        val sequence = parseSequence(outer.seq)
        if (sequence != nextReceiveSequence) throw EnvelopeException("sequence_violation")
        val nonce = decodeFixed(outer.nonceB64, NONCE_BYTES, "invalid_nonce")
        val requestIdBytes = decodeFixed(outer.requestIdB64, REQUEST_ID_BYTES, "invalid_request_id")
        val payload = decodeUrl(outer.payloadB64, "invalid_payload")
        if (payload.size > MAX_INNER_PAYLOAD_BYTES) throw EnvelopeException("payload_too_large")
        val mac = decodeFixed(outer.macB64, MAC_BYTES, "invalid_mac")
        validateOperation(outer.op)
        val expected = hmac(
            receiveKey,
            transcript(outer.channel, sequence, nonce, requestIdBytes, outer.op, payload),
        )
        if (!MessageDigest.isEqual(expected, mac)) throw EnvelopeException("mac_rejected")
        if (nextReceiveSequence == ULong.MAX_VALUE) throw EnvelopeException("sequence_exhausted")
        nextReceiveSequence++
        return VerifiedEnvelope(
            channel = outer.channel,
            sequence = sequence,
            requestId = bytesToUuid(requestIdBytes),
            operation = outer.op,
            payload = payload,
        )
    }

    private fun encode(
        channel: Int,
        sequence: ULong,
        requestId: UUID,
        operation: String,
        payload: ByteArray,
        key: ByteArray,
    ): ByteArray {
        validateOperation(operation)
        if (payload.size > MAX_INNER_PAYLOAD_BYTES) throw EnvelopeException("payload_too_large")
        val nonce = ByteArray(NONCE_BYTES).also(random::nextBytes)
        val requestIdBytes = uuidToBytes(requestId)
        val mac = hmac(key, transcript(channel, sequence, nonce, requestIdBytes, operation, payload))
        val outer = SecureOuterEnvelope(
            v = 1,
            channel = channel,
            seq = sequence.toString(),
            nonceB64 = encodeUrl(nonce),
            requestIdB64 = encodeUrl(requestIdBytes),
            op = operation,
            payloadB64 = encodeUrl(payload),
            macB64 = encodeUrl(mac),
        )
        val encoded = json.encodeToString(outer).toByteArray(Charsets.UTF_8)
        if (encoded.size > MAX_GATT_JSON_BYTES) throw EnvelopeException("frame_too_large")
        return encoded
    }

    private fun requireExactOuterKeys(encoded: String) {
        // Every envelope value is a scalar and all string alphabets are
        // constrained (base64url/op/decimal), so a key token cannot occur
        // inside a valid value. Count before deserialization because
        // kotlinx.serialization otherwise accepts a duplicate key by keeping
        // the final value.
        val keys = OUTER_KEY.findAll(encoded).map { it.groupValues[1] }.toList()
        if (keys.size != OUTER_KEYS.size || keys.toSet() != OUTER_KEYS) {
            throw EnvelopeException("malformed_envelope")
        }
    }

    companion object {
        fun derive(
            controllerRoot: ByteArray,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
            random: SecureRandom = SecureRandom(),
        ): SecureEnvelopeSession {
            require(controllerRoot.size == 32) { "controller_root_must_be_32_bytes" }
            require(clientNonce.size == NONCE_BYTES) { "client_nonce_must_be_16_bytes" }
            require(deviceNonce.size == NONCE_BYTES) { "device_nonce_must_be_16_bytes" }
            val (c2d, d2c) = deriveKeys(controllerRoot, clientNonce, deviceNonce)
            return SecureEnvelopeSession(c2d, d2c, random, setOf(CHANNEL_RESPONSE, CHANNEL_EVENT))
        }

        internal fun deriveForDevice(
            controllerRoot: ByteArray,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
            random: SecureRandom = SecureRandom(),
        ): SecureEnvelopeSession {
            val (c2d, d2c) = deriveKeys(controllerRoot, clientNonce, deviceNonce)
            return SecureEnvelopeSession(d2c, c2d, random, setOf(CHANNEL_REQUEST))
        }

        private fun deriveKeys(
            controllerRoot: ByteArray,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
        ): Pair<ByteArray, ByteArray> {
            require(controllerRoot.size == 32) { "controller_root_must_be_32_bytes" }
            require(clientNonce.size == NONCE_BYTES) { "client_nonce_must_be_16_bytes" }
            require(deviceNonce.size == NONCE_BYTES) { "device_nonce_must_be_16_bytes" }
            val salt = MessageDigest.getInstance("SHA-256").digest(clientNonce + deviceNonce)
            val prk = hmac(salt, controllerRoot)
            return hkdfExpand(prk, C2D_INFO.toByteArray(Charsets.US_ASCII), 32) to
                hkdfExpand(prk, D2C_INFO.toByteArray(Charsets.US_ASCII), 32)
        }

        internal fun transcript(
            channel: Int,
            sequence: ULong,
            nonce: ByteArray,
            requestId: ByteArray,
            operation: String,
            payload: ByteArray,
        ): ByteArray {
            require(channel in CHANNEL_REQUEST..CHANNEL_EVENT)
            require(nonce.size == NONCE_BYTES)
            require(requestId.size == REQUEST_ID_BYTES)
            val op = operation.toByteArray(Charsets.US_ASCII)
            val output = ByteArrayOutputStream(
                TRANSCRIPT_PREFIX.size + 1 + 8 + NONCE_BYTES + REQUEST_ID_BYTES + 2 + op.size + 4 + payload.size,
            )
            output.write(TRANSCRIPT_PREFIX)
            output.write(channel)
            output.write(ulongBytes(sequence))
            output.write(nonce)
            output.write(requestId)
            output.write(byteArrayOf((op.size ushr 8).toByte(), op.size.toByte()))
            output.write(op)
            output.write(
                byteArrayOf(
                    (payload.size ushr 24).toByte(),
                    (payload.size ushr 16).toByte(),
                    (payload.size ushr 8).toByte(),
                    payload.size.toByte(),
                ),
            )
            output.write(payload)
            return output.toByteArray()
        }

        internal fun hmac(key: ByteArray, data: ByteArray): ByteArray =
            Mac.getInstance("HmacSHA256").run {
                init(SecretKeySpec(key, "HmacSHA256"))
                doFinal(data)
            }

        internal fun hkdfExpand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
            require(length in 1..32) { "single_block_hkdf_only" }
            return hmac(prk, info + byteArrayOf(1)).copyOf(length)
        }

        internal fun encodeUrl(bytes: ByteArray): String =
            Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

        internal fun decodeUrl(text: String, code: String): ByteArray = try {
            require(URL_BASE64.matches(text))
            Base64.getUrlDecoder().decode(text).also { require(encodeUrl(it) == text) }
        } catch (failure: Throwable) {
            throw EnvelopeException(code, failure)
        }

        private fun decodeFixed(text: String, size: Int, code: String): ByteArray =
            decodeUrl(text, code).also { if (it.size != size) throw EnvelopeException(code) }

        private fun parseSequence(text: String): ULong {
            if (!SEQUENCE.matches(text)) throw EnvelopeException("invalid_sequence")
            return text.toULongOrNull()?.takeIf { it > 0u } ?: throw EnvelopeException("invalid_sequence")
        }

        private fun validateOperation(operation: String) {
            if (!OPERATION.matches(operation)) throw EnvelopeException("invalid_operation")
        }

        private fun uuidToBytes(uuid: UUID): ByteArray = ByteBuffer.allocate(16)
            .order(ByteOrder.BIG_ENDIAN)
            .putLong(uuid.mostSignificantBits)
            .putLong(uuid.leastSignificantBits)
            .array()

        private fun bytesToUuid(bytes: ByteArray): UUID = ByteBuffer.wrap(bytes)
            .order(ByteOrder.BIG_ENDIAN)
            .let { UUID(it.long, it.long) }

        private fun ulongBytes(value: ULong): ByteArray = ByteArray(8) { index ->
            (value shr ((7 - index) * 8)).toByte()
        }

        const val CHANNEL_REQUEST = 0
        const val CHANNEL_RESPONSE = 1
        const val CHANNEL_EVENT = 2
        const val NONCE_BYTES = 16
        const val REQUEST_ID_BYTES = 16
        const val MAC_BYTES = 32
        const val MAX_INNER_PAYLOAD_BYTES = 12_000
        private val OUTER_KEYS = setOf(
            "v", "channel", "seq", "nonce_b64", "request_id_b64", "op", "payload_b64", "mac_b64",
        )
        private val OUTER_KEY = Regex("\\\"([A-Za-z0-9_]+)\\\"\\s*:")
        private const val C2D_INFO = "kitsu868/ble/c2d/v1"
        private const val D2C_INFO = "kitsu868/ble/d2c/v1"
        private val TRANSCRIPT_PREFIX = "KITSU-ENV-1\u0000".toByteArray(Charsets.US_ASCII)
        private val OPERATION = Regex("^[A-Za-z0-9._/-]{1,48}$")
        private val SEQUENCE = Regex("^[1-9][0-9]{0,19}$")
        private val URL_BASE64 = Regex("^[A-Za-z0-9_-]*$")
    }
}

class EnvelopeException(val code: String, cause: Throwable? = null) : Exception(code, cause)
