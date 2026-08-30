package ptl.kitsu.app.pairing

import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.security.ControllerRole
import ptl.kitsu.app.transport.SecureEnvelopeSession

class ControllerCaretakerPairingProtocolTest {
    @Test fun roleBoundProofsMatchFrozenFirmwareVectors() {
        val root = ByteArray(32) { it.toByte() }
        val controller = ByteArray(16) { it.toByte() }
        val client = ByteArray(16) { (it + 16).toByte() }
        val device = ByteArray(16) { (it + 32).toByte() }

        assertEquals(
            "1cef77337d81072efb9ed54a921c30fd3c01a092672f947eeaf103772658b15a",
            v2Proof(root, "device", "caretaker", controller, client, device).hex(),
        )
        assertEquals(
            "8e0977742ba1aab1c0bb2795147ac2e22f6509fb751e99d6000de8e952598e87",
            v2Proof(root, "client", "caretaker", controller, client, device).hex(),
        )
        assertEquals(
            "ac26365b6b415d9698931ab15a8fe02a2a56cd8016919d7119b41d1d4e628451",
            v2Proof(root, "ok", "caretaker", controller, client, device).hex(),
        )
        assertFalse(
            MessageDigest.isEqual(
                v2Proof(root, "device", "caretaker", controller, client, device),
                v2Proof(root, "device", "owner", controller, client, device),
            ),
        )
    }

    @Test fun explicitCaretakerFlowSendsNoRoleSelectorAndPersistsAttestedRole() = runTest {
        var stored: ControllerGrant? = null
        val channel = CaretakerDeviceChannel(candidateStored = { stored != null })

        val result = ControllerPairingProtocol(FixedRandom()).pair(
            label = "Family phone",
            channel = channel,
            persistCandidate = { stored = it },
            deleteCandidate = { stored = null },
            flow = ControllerPairingFlow.CARETAKER,
        )

        assertEquals(ControllerRole.CARETAKER, result.role)
        assertEquals(result, stored)
        assertEquals(setOf("v", "type", "client_nonce_b64", "label", "platform"), channel.requestKeys)
        assertFalse("phone must not select a role", "role" in channel.requestKeys)
        assertTrue(channel.clientCommitVerified)
    }

    @Test fun caretakerRejectsVersionDowngradeBeforeCredentialPersistence() = runTest {
        var stored: ControllerGrant? = null
        val channel = CaretakerDeviceChannel(pendingVersion = 1, pendingRole = null)

        val failure = runCatching {
            ControllerPairingProtocol(FixedRandom()).pair(
                label = "Family phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null },
                flow = ControllerPairingFlow.CARETAKER,
            )
        }.exceptionOrNull() as PairingException

        assertEquals("pairing_failed", failure.code)
        assertNull(stored)
    }

    @Test fun caretakerRejectsMissingInjectedOrOwnerRole() = runTest {
        listOf<String?>(null, "owner", "admin").forEach { suppliedRole ->
            var stored: ControllerGrant? = null
            val failure = runCatching {
                ControllerPairingProtocol(FixedRandom()).pair(
                    label = "Family phone",
                    channel = CaretakerDeviceChannel(pendingRole = suppliedRole),
                    persistCandidate = { stored = it },
                    deleteCandidate = { stored = null },
                    flow = ControllerPairingFlow.CARETAKER,
                )
            }.exceptionOrNull() as PairingException
            assertEquals("pairing_failed", failure.code)
            assertNull(stored)
        }
    }

    @Test fun roleTamperedDeviceProofIsRejected() = runTest {
        var stored: ControllerGrant? = null
        val failure = runCatching {
            ControllerPairingProtocol(FixedRandom()).pair(
                label = "Family phone",
                channel = CaretakerDeviceChannel(deviceProofRole = "owner"),
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null },
                flow = ControllerPairingFlow.CARETAKER,
            )
        }.exceptionOrNull() as PairingException

        assertEquals("pairing_failed", failure.code)
        assertNull(stored)
    }

    @Test fun caretakerRejectsV2OwnerRoleInGrantAndOk() = runTest {
        val grantMismatch = runCatching {
            ControllerPairingProtocol(FixedRandom()).pair(
                label = "Family phone",
                channel = CaretakerDeviceChannel(grantRole = "owner"),
                persistCandidate = {},
                deleteCandidate = {},
                flow = ControllerPairingFlow.CARETAKER,
            )
        }.exceptionOrNull() as PairingException
        assertEquals("pairing_failed", grantMismatch.code)

        var stored: ControllerGrant? = null
        val okMismatch = runCatching {
            ControllerPairingProtocol(FixedRandom()).pair(
                label = "Family phone",
                channel = CaretakerDeviceChannel(
                    candidateStored = { stored != null },
                    okRole = "owner",
                ),
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null },
                flow = ControllerPairingFlow.CARETAKER,
            )
        }.exceptionOrNull() as PairingException
        assertEquals("pairing_failed", okMismatch.code)
    }

    @Test fun ownerV1RequestAndCommitRemainByteForByteCompatible() = runTest {
        val channel = OwnerWireChannel()
        val grant = ControllerPairingProtocol(FixedRandom()).pair(
            label = "Owner phone",
            channel = channel,
            persistCandidate = {},
            deleteCandidate = {},
        )

        assertEquals(ControllerRole.OWNER, grant.role)
        assertEquals(
            "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"AAAAAAAAAAAAAAAAAAAAAA\",\"label\":\"Owner phone\",\"platform\":\"android\"}",
            channel.sent[0],
        )
        val expectedClient = ControllerPairingProtocol.proof(
            channel.root,
            ControllerPairingProtocol.CLIENT_PREFIX,
            channel.controllerId,
            channel.deviceUid,
            ByteArray(16),
            channel.deviceNonce,
        )
        assertEquals(
            "{\"v\":1,\"type\":\"pair_commit\",\"proof_b64\":\"${SecureEnvelopeSession.encodeUrl(expectedClient)}\"}",
            channel.sent[1],
        )
    }

    private fun v2Proof(
        root: ByteArray,
        proofRole: String,
        controllerRole: String,
        controller: ByteArray,
        client: ByteArray,
        device: ByteArray,
    ) = ControllerPairingProtocol.roleBoundProof(
        root,
        proofRole.toByteArray(Charsets.US_ASCII),
        controllerRole.toByteArray(Charsets.US_ASCII),
        controller,
        "KT1234",
        client,
        device,
    )

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }

    private class FixedRandom : SecureRandom() {
        override fun nextBytes(bytes: ByteArray) = bytes.fill(0)
    }

    private class CaretakerDeviceChannel(
        private val candidateStored: () -> Boolean = { false },
        private val pendingVersion: Int = 2,
        private val pendingRole: String? = "caretaker",
        private val grantVersion: Int = 2,
        private val grantRole: String? = "caretaker",
        private val deviceProofRole: String = "caretaker",
        private val okVersion: Int = 2,
        private val okRole: String? = "caretaker",
    ) : PairingChannel {
        private val json = Json
        private val replies = Channel<ByteArray>(Channel.UNLIMITED)
        private val root = ByteArray(32) { (it + 64).toByte() }
        private val controllerId = ByteArray(16) { (it + 16).toByte() }
        private val deviceNonce = ByteArray(16) { (it + 48).toByte() }
        private val deviceUid = "KT1234"
        private lateinit var clientNonce: ByteArray
        var requestKeys: Set<String> = emptySet()
            private set
        var clientCommitVerified = false
            private set

        override suspend fun send(payload: ByteArray) {
            val value = json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
            when (value.getValue("type").jsonPrimitive.content) {
                "pair_request" -> {
                    requestKeys = value.keys
                    assertEquals(2, value.getValue("v").jsonPrimitive.content.toInt())
                    clientNonce = SecureEnvelopeSession.decodeUrl(
                        value.getValue("client_nonce_b64").jsonPrimitive.content,
                        "test",
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", pendingVersion)
                            put("type", "pair_pending")
                            pendingRole?.let { put("role", it) }
                            put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                            put("expires_in_ms", 60_000)
                        }.toString().toByteArray(),
                    )
                    val proof = ControllerPairingProtocol.roleBoundProof(
                        root,
                        "device".toByteArray(),
                        deviceProofRole.toByteArray(),
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", grantVersion)
                            put("type", "pair_grant")
                            grantRole?.let { put("role", it) }
                            put("controller_id_b64", SecureEnvelopeSession.encodeUrl(controllerId))
                            put("root_b64", SecureEnvelopeSession.encodeUrl(root))
                            put("device_uid", deviceUid)
                            put("client_nonce_b64", SecureEnvelopeSession.encodeUrl(clientNonce))
                            put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                            put("proof_b64", SecureEnvelopeSession.encodeUrl(proof))
                        }.toString().toByteArray(),
                    )
                }
                "pair_commit" -> {
                    assertTrue("candidate must be stored before pair_commit", candidateStored())
                    assertEquals(2, value.getValue("v").jsonPrimitive.content.toInt())
                    assertEquals("caretaker", value.getValue("role").jsonPrimitive.content)
                    val expected = ControllerPairingProtocol.roleBoundProof(
                        root,
                        "client".toByteArray(),
                        "caretaker".toByteArray(),
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    val actual = SecureEnvelopeSession.decodeUrl(
                        value.getValue("proof_b64").jsonPrimitive.content,
                        "test",
                    )
                    clientCommitVerified = MessageDigest.isEqual(expected, actual)
                    assertTrue(clientCommitVerified)
                    val ok = ControllerPairingProtocol.roleBoundProof(
                        root,
                        "ok".toByteArray(),
                        "caretaker".toByteArray(),
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", okVersion)
                            put("type", "pair_ok")
                            okRole?.let { put("role", it) }
                            put("proof_b64", SecureEnvelopeSession.encodeUrl(ok))
                        }.toString().toByteArray(),
                    )
                }
            }
        }

        override suspend fun receive(): ByteArray = replies.receive()
    }

    private class OwnerWireChannel : PairingChannel {
        private val json = Json
        private val replies = Channel<ByteArray>(Channel.UNLIMITED)
        val root = ByteArray(32) { (it + 64).toByte() }
        val controllerId = ByteArray(16) { (it + 16).toByte() }
        val deviceNonce = ByteArray(16) { (it + 48).toByte() }
        val deviceUid = "KT1234"
        val sent = mutableListOf<String>()
        private lateinit var clientNonce: ByteArray

        override suspend fun send(payload: ByteArray) {
            val text = payload.toString(Charsets.UTF_8)
            sent += text
            val value = json.parseToJsonElement(text).jsonObject
            when (value.getValue("type").jsonPrimitive.content) {
                "pair_request" -> {
                    clientNonce = SecureEnvelopeSession.decodeUrl(
                        value.getValue("client_nonce_b64").jsonPrimitive.content,
                        "test",
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", 1)
                            put("type", "pair_pending")
                            put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                            put("expires_in_ms", 60_000)
                        }.toString().toByteArray(),
                    )
                    val deviceProof = ControllerPairingProtocol.proof(
                        root,
                        ControllerPairingProtocol.DEVICE_PREFIX,
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", 1)
                            put("type", "pair_grant")
                            put("controller_id_b64", SecureEnvelopeSession.encodeUrl(controllerId))
                            put("root_b64", SecureEnvelopeSession.encodeUrl(root))
                            put("device_uid", deviceUid)
                            put("client_nonce_b64", SecureEnvelopeSession.encodeUrl(clientNonce))
                            put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                            put("proof_b64", SecureEnvelopeSession.encodeUrl(deviceProof))
                        }.toString().toByteArray(),
                    )
                }
                "pair_commit" -> {
                    val okProof = ControllerPairingProtocol.proof(
                        root,
                        ControllerPairingProtocol.OK_PREFIX,
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", 1)
                            put("type", "pair_ok")
                            put("proof_b64", SecureEnvelopeSession.encodeUrl(okProof))
                        }.toString().toByteArray(),
                    )
                }
            }
        }

        override suspend fun receive(): ByteArray = replies.receive()
    }
}
