package ptl.kitsu.app.transport

import java.util.ArrayDeque
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertArrayEquals
import org.junit.Test

class CapabilityHandshakeTest {
    @Test fun firmwareHandshakeGoldenProofsMatchAllRoles() {
        val root = ByteArray(32) { it.toByte() }
        val controllerId = ByteArray(16) { it.toByte() }
        val clientNonce = ByteArray(16) { (it + 16).toByte() }
        val deviceNonce = ByteArray(16) { (it + 32).toByte() }

        assertEquals(
            "34dcf7054c7935d925cdd3f8c22299ef98f3cd244e43ce4588152269d1eb880b",
            CapabilityHandshake.proof(
                root, CapabilityHandshake.DEVICE_PREFIX, controllerId, clientNonce, deviceNonce,
            ).hex(),
        )
        assertEquals(
            "53417d679ba763a971db1df280fac91c2fd2a07af657e3976a5d4dd71777d1e3",
            CapabilityHandshake.proof(
                root, CapabilityHandshake.CLIENT_PREFIX, controllerId, clientNonce, deviceNonce,
            ).hex(),
        )
        assertEquals(
            "aef84053de4d6cf78b1e0df3833c36d795e4ca367fe73a3a8c312e7a7c1d6723",
            CapabilityHandshake.proof(
                root, CapabilityHandshake.OK_PREFIX, controllerId, clientNonce, deviceNonce,
            ).hex(),
        )

        val clientSession = SecureEnvelopeSession.derive(root, clientNonce, deviceNonce)
        val deviceSession = SecureEnvelopeSession.deriveForDevice(root, clientNonce, deviceNonce)
        val request = clientSession.encodeRequest(
            java.util.UUID(0, 0), "state.get", "{}".toByteArray(),
        )
        assertArrayEquals("{}".toByteArray(), deviceSession.decodeIncoming(request).payload)
    }

    @Test fun mutualProofProducesUsableSession() = runTest {
        val root = ByteArray(32) { (it + 1).toByte() }
        val controllerId = ByteArray(16) { (it + 7).toByte() }
        val deviceNonce = ByteArray(16) { (it + 33).toByte() }
        val json = Json
        var clientNonce = ByteArray(0)
        var step = 0
        val session = CapabilityHandshake().perform(controllerId, root) { request ->
            val input = json.parseToJsonElement(request.toString(Charsets.UTF_8)).jsonObject
            when (step++) {
                0 -> {
                    assertEquals("client_hello", input.getValue("type").jsonPrimitive.content)
                    clientNonce = SecureEnvelopeSession.decodeUrl(
                        input.getValue("client_nonce_b64").jsonPrimitive.content,
                        "bad",
                    )
                    val proof = CapabilityHandshake.proof(
                        root,
                        CapabilityHandshake.DEVICE_PREFIX,
                        controllerId,
                        clientNonce,
                        deviceNonce,
                    )
                    buildJsonObject {
                        put("v", 1)
                        put("type", "device_hello")
                        put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                        put("proof_b64", SecureEnvelopeSession.encodeUrl(proof))
                    }.toString().toByteArray()
                }
                else -> {
                    val received = SecureEnvelopeSession.decodeUrl(
                        input.getValue("proof_b64").jsonPrimitive.content,
                        "bad",
                    )
                    val expected = CapabilityHandshake.proof(
                        root,
                        CapabilityHandshake.CLIENT_PREFIX,
                        controllerId,
                        clientNonce,
                        deviceNonce,
                    )
                    assertEquals(SecureEnvelopeSession.encodeUrl(expected), SecureEnvelopeSession.encodeUrl(received))
                    val proof = CapabilityHandshake.proof(
                        root,
                        CapabilityHandshake.OK_PREFIX,
                        controllerId,
                        clientNonce,
                        deviceNonce,
                    )
                    buildJsonObject {
                        put("v", 1)
                        put("type", "device_ok")
                        put("proof_b64", SecureEnvelopeSession.encodeUrl(proof))
                    }.toString().toByteArray()
                }
            }
        }
        assertEquals(2, step)
        val request = session.encodeRequest(java.util.UUID.randomUUID(), "state.get", "{}".toByteArray())
        assert(request.isNotEmpty())
    }

    @Test fun exactFirmwareAuthErrorIsAuthoritativeControllerRejection() = runTest {
        val failure = runCatching {
            CapabilityHandshake().perform(ByteArray(16) { 1 }, ByteArray(32) { 2 }) {
                """{"v":1,"type":"error","code":"auth_failed"}""".toByteArray()
            }
        }.exceptionOrNull() as HandshakeException
        assertEquals("controller_rejected", failure.code)
    }

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }
}
