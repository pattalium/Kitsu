package app.kitsu.mobile.transport

import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

@Serializable
private data class ClientHello(
    val v: Int = 1,
    val type: String = "client_hello",
    @SerialName("controller_id_b64") val controllerIdB64: String,
    @SerialName("client_nonce_b64") val clientNonceB64: String,
)

@Serializable
private data class DeviceHello(
    val v: Int,
    val type: String,
    @SerialName("device_nonce_b64") val deviceNonceB64: String,
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class ClientAuth(
    val v: Int = 1,
    val type: String = "client_auth",
    @SerialName("proof_b64") val proofB64: String,
)

@Serializable
private data class DeviceOk(
    val v: Int,
    val type: String,
    @SerialName("proof_b64") val proofB64: String,
)

class CapabilityHandshake(private val random: SecureRandom = SecureRandom()) {
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = false
        // v/type are protocol constants with Kotlin defaults. They still have
        // to be present on the wire so the device can reject other schemas.
        encodeDefaults = true
    }

    suspend fun perform(
        controllerId: ByteArray,
        controllerRoot: ByteArray,
        exchange: suspend (ByteArray) -> ByteArray,
    ): SecureEnvelopeSession {
        if (controllerId.size != 16 || controllerRoot.size != 32) {
            throw HandshakeException("invalid_controller_capability")
        }
        val clientNonce = ByteArray(16).also(random::nextBytes)
        val helloBytes = json.encodeToString(
            ClientHello(
                controllerIdB64 = SecureEnvelopeSession.encodeUrl(controllerId),
                clientNonceB64 = SecureEnvelopeSession.encodeUrl(clientNonce),
            ),
        ).toByteArray(Charsets.UTF_8)
        requireHandshakeSize(helloBytes)

        val deviceHelloBytes = exchange(helloBytes)
        requireHandshakeSize(deviceHelloBytes)
        val deviceHello = decodeDeviceHello(deviceHelloBytes)
        val deviceNonce = decodeFixed(deviceHello.deviceNonceB64, 16, "invalid_device_nonce")
        val expectedDevice = proof(controllerRoot, DEVICE_PREFIX, controllerId, clientNonce, deviceNonce)
        val actualDevice = decodeFixed(deviceHello.proofB64, 32, "invalid_device_proof")
        if (!MessageDigest.isEqual(expectedDevice, actualDevice)) {
            throw HandshakeException("auth_failed")
        }

        val clientProof = proof(controllerRoot, CLIENT_PREFIX, controllerId, clientNonce, deviceNonce)
        val authBytes = json.encodeToString(
            ClientAuth(proofB64 = SecureEnvelopeSession.encodeUrl(clientProof)),
        ).toByteArray(Charsets.UTF_8)
        val okBytes = exchange(authBytes)
        requireHandshakeSize(okBytes)
        val ok = decodeDeviceOk(okBytes)
        val expectedOk = proof(controllerRoot, OK_PREFIX, controllerId, clientNonce, deviceNonce)
        val actualOk = decodeFixed(ok.proofB64, 32, "invalid_ok_proof")
        if (!MessageDigest.isEqual(expectedOk, actualOk)) throw HandshakeException("auth_failed")

        return SecureEnvelopeSession.derive(controllerRoot, clientNonce, deviceNonce, random)
    }

    private fun decodeDeviceHello(bytes: ByteArray): DeviceHello = try {
        json.decodeFromString(DeviceHello.serializer(), bytes.toString(Charsets.UTF_8)).also {
            if (it.v != 1 || it.type != "device_hello") throw HandshakeException("auth_failed")
        }
    } catch (failure: HandshakeException) {
        throw failure
    } catch (failure: Throwable) {
        throw HandshakeException("auth_failed", failure)
    }

    private fun decodeDeviceOk(bytes: ByteArray): DeviceOk = try {
        json.decodeFromString(DeviceOk.serializer(), bytes.toString(Charsets.UTF_8)).also {
            if (it.v != 1 || it.type != "device_ok") throw HandshakeException("auth_failed")
        }
    } catch (failure: HandshakeException) {
        throw failure
    } catch (failure: Throwable) {
        throw HandshakeException("auth_failed", failure)
    }

    companion object {
        internal fun proof(
            root: ByteArray,
            prefix: ByteArray,
            controllerId: ByteArray,
            clientNonce: ByteArray,
            deviceNonce: ByteArray,
        ): ByteArray = SecureEnvelopeSession.hmac(
            root,
            prefix + controllerId + clientNonce + deviceNonce,
        )

        private fun decodeFixed(text: String, size: Int, code: String): ByteArray =
            SecureEnvelopeSession.decodeUrl(text, code).also {
                if (it.size != size) throw HandshakeException(code)
            }

        private fun requireHandshakeSize(bytes: ByteArray) {
            if (bytes.isEmpty() || bytes.size > MAX_HANDSHAKE_BYTES) {
                throw HandshakeException("invalid_handshake_frame")
            }
        }

        internal val DEVICE_PREFIX = "KITSU-HS-1\u0000device\u0000".toByteArray(Charsets.US_ASCII)
        internal val CLIENT_PREFIX = "KITSU-HS-1\u0000client\u0000".toByteArray(Charsets.US_ASCII)
        internal val OK_PREFIX = "KITSU-HS-1\u0000ok\u0000".toByteArray(Charsets.US_ASCII)
        const val MAX_HANDSHAKE_BYTES = 1024
        const val HANDSHAKE_TIMEOUT_MILLIS = 10_000L
    }
}

class HandshakeException(val code: String, cause: Throwable? = null) : Exception(code, cause)
