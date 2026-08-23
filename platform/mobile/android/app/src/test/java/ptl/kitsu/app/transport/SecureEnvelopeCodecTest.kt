package ptl.kitsu.app.transport

import java.util.UUID
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class SecureEnvelopeCodecTest {
    private val root = ByteArray(32) { it.toByte() }
    private val clientNonce = ByteArray(16) { (it + 16).toByte() }
    private val deviceNonce = ByteArray(16) { (it + 32).toByte() }

    @Test fun directionsDeriveInteroperableKeysAndBindRequest() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val id = UUID.randomUUID()
        val payload = "{\"v\":1}".toByteArray()
        val request = client.encodeRequest(id, "state.get", payload)
        assertTrue(request.toString(Charsets.UTF_8).contains("\"v\":1"))
        val decodedRequest = device.decodeIncoming(request)
        assertEquals(SecureEnvelopeSession.CHANNEL_REQUEST, decodedRequest.channel)
        assertEquals(1uL, decodedRequest.sequence)
        assertEquals(id, decodedRequest.requestId)
        assertArrayEquals(payload, decodedRequest.payload)

        val response = device.encodeOutgoing(
            SecureEnvelopeSession.CHANNEL_RESPONSE,
            id,
            "state.get",
            "{\"ok\":true}".toByteArray(),
        )
        val decodedResponse = client.decodeIncoming(response)
        assertEquals(id, decodedResponse.requestId)
        assertEquals("state.get", decodedResponse.operation)
    }

    @Test fun modifiedPayloadFailsMacBeforeInnerParsing() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val encoded = device.encodeOutgoing(
            SecureEnvelopeSession.CHANNEL_EVENT,
            UUID(0, 0),
            "message",
            "{}".toByteArray(),
        )
        val json = Json
        val outer = json.decodeFromString(SecureOuterEnvelope.serializer(), encoded.toString(Charsets.UTF_8))
        val tampered = outer.copy(payloadB64 = SecureEnvelopeSession.encodeUrl("[]".toByteArray()))
        val bytes = json.encodeToString(tampered).toByteArray()
        val failure = assertThrows(EnvelopeException::class.java) { client.decodeIncoming(bytes) }
        assertEquals("mac_rejected", failure.code)
    }

    @Test fun controllerOperationAndSensitiveBodyAreMacBound() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val payload = "{\"private_value\":\"test-secret\"}".toByteArray()
        val encoded = client.encodeRequest(UUID.randomUUID(), "firmware.update.begin", payload)

        // The GATT envelope is authenticated and base64url-wrapped; neither
        // transient input appears as plaintext in logs or packet framing.
        val outerText = encoded.toString(Charsets.UTF_8)
        assertTrue(!outerText.contains("test-secret"))
        val decoded = device.decodeIncoming(encoded)
        assertEquals("firmware.update.begin", decoded.operation)
        assertArrayEquals(payload, decoded.payload)

        val tamperTarget = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val operationTampered = outerText.replace(
            "\"op\":\"firmware.update.begin\"",
            "\"op\":\"state.get\"",
        ).toByteArray()
        assertEquals(
            "mac_rejected",
            assertThrows(EnvelopeException::class.java) {
                tamperTarget.decodeIncoming(operationTampered)
            }.code,
        )
    }

    @Test fun exactSequenceRejectsSkippedFrame() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        device.encodeOutgoing(SecureEnvelopeSession.CHANNEL_EVENT, UUID(0, 0), "status", "{}".toByteArray())
        val second = device.encodeOutgoing(SecureEnvelopeSession.CHANNEL_EVENT, UUID(0, 0), "status", "{}".toByteArray())
        val failure = assertThrows(EnvelopeException::class.java) { client.decodeIncoming(second) }
        assertEquals("sequence_violation", failure.code)
    }

    @Test fun missingOuterVersionIsRejectedInsteadOfDefaulted() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val valid = device.encodeOutgoing(
            SecureEnvelopeSession.CHANNEL_EVENT,
            UUID(0, 0),
            "status",
            "{}".toByteArray(),
        ).toString(Charsets.UTF_8)
        val missingVersion = valid.replaceFirst("\"v\":1,", "").toByteArray()
        val failure = assertThrows(EnvelopeException::class.java) {
            client.decodeIncoming(missingVersion)
        }
        assertEquals("malformed_envelope", failure.code)
    }

    @Test fun duplicateOrUnknownOuterKeysAreRejectedBeforeDecoding() {
        val client = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val device = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val valid = device.encodeOutgoing(
            SecureEnvelopeSession.CHANNEL_EVENT,
            UUID(0, 0),
            "status",
            "{}".toByteArray(),
        ).toString(Charsets.UTF_8)

        val duplicate = valid.replaceFirst("{", "{\"v\":1,").toByteArray()
        assertEquals(
            "malformed_envelope",
            assertThrows(EnvelopeException::class.java) { client.decodeIncoming(duplicate) }.code,
        )
        val unknown = valid.replaceFirst("{", "{\"extra\":0,").toByteArray()
        assertEquals(
            "malformed_envelope",
            assertThrows(EnvelopeException::class.java) { client.decodeIncoming(unknown) }.code,
        )
    }

    @Test fun base64UrlMustUseCanonicalUnpaddedEncoding() {
        val failure = assertThrows(EnvelopeException::class.java) {
            SecureEnvelopeSession.decodeUrl("AB", "invalid_payload")
        }
        assertEquals("invalid_payload", failure.code)
    }
}
