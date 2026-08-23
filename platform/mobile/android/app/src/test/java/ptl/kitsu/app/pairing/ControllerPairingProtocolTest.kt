package ptl.kitsu.app.pairing

import ptl.kitsu.app.transport.SecureEnvelopeSession
import java.security.MessageDigest
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerPairingProtocolTest {
    @Test fun proofTranscriptsBindRoleControllerAndBothNonces() {
        val root = ByteArray(32) { it.toByte() }
        val controller = ByteArray(16) { it.toByte() }
        val client = ByteArray(16) { (it + 16).toByte() }
        val device = ByteArray(16) { (it + 32).toByte() }

        assertEquals(
            "fe29b40817a936ad8c66145656e7c1bd92e1e6588002058141b00339d0cc5c0e",
            ControllerPairingProtocol.proof(
                root, ControllerPairingProtocol.DEVICE_PREFIX, controller, "KT1234", client, device,
            ).hex(),
        )
        assertEquals(
            "70e68e6965dde64a1f444e0c92990fac568e6c8e64e8e8e94b18ea160e2e8df7",
            ControllerPairingProtocol.proof(
                root, ControllerPairingProtocol.CLIENT_PREFIX, controller, "KT1234", client, device,
            ).hex(),
        )
        assertEquals(
            "25567efa6667c05795b728eebb3811109662e3f88a7c709114cfef0b7b2411bc",
            ControllerPairingProtocol.proof(
                root, ControllerPairingProtocol.OK_PREFIX, controller, "KT1234", client, device,
            ).hex(),
        )
        val mutatedRoot = root.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
        assertFalse(
            MessageDigest.isEqual(
                ControllerPairingProtocol.proof(
                    root, ControllerPairingProtocol.DEVICE_PREFIX, controller, "KT1234", client, device,
                ),
                ControllerPairingProtocol.proof(
                    mutatedRoot, ControllerPairingProtocol.DEVICE_PREFIX, controller, "KT1234", client, device,
                ),
            ),
        )
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun mockGattChannelCommitsOnlyAfterVerifiedPairOk() = runTest {
        var stored: ControllerGrant? = null
        var deleted = false
        var pendingSeen = false
        var storedProgressSeen = false
        val channel = MockDeviceChannel(candidateStored = { stored != null })

        val result = ControllerPairingProtocol().pair(
            label = "Owner phone",
            channel = channel,
            persistCandidate = { stored = it },
            deleteCandidate = { stored = null; deleted = true },
            onPending = { pendingSeen = it in 1..60_000 },
            onCandidateStored = { storedProgressSeen = stored != null },
        )

        assertEquals(result, stored)
        assertTrue(channel.commitVerified)
        assertTrue(pendingSeen)
        assertTrue(storedProgressSeen)
        assertFalse(deleted)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun invalidPairOkRetainsCandidateForAuthenticatedRecovery() = runTest {
        var stored: ControllerGrant? = null
        var deleted = false
        val channel = MockDeviceChannel(candidateStored = { stored != null }, corruptOK = true)

        val failure = runCatching {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null; deleted = true },
            )
        }.exceptionOrNull() as PairingException
        assertEquals("pairing_failed", failure.code)
        assertNotNull(stored)
        assertFalse(deleted)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun processLikeCancellationAfterCommitRetainsCandidate() = runTest {
        var stored: ControllerGrant? = null
        var deleted = false
        val channel = MockDeviceChannel(candidateStored = { stored != null }, withholdOK = true)
        val operation = launch {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null; deleted = true },
            )
        }

        channel.commitSeen.await()
        assertNotNull(stored)
        operation.cancel()
        operation.join()

        assertNotNull(stored)
        assertFalse(deleted)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun lostPairOkRetainsCandidateAcrossTimeout() = runTest {
        var stored: ControllerGrant? = null
        var deleted = false
        val channel = MockDeviceChannel(candidateStored = { stored != null }, withholdOK = true)
        val failure = runCatching {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null; deleted = true },
            )
        }.exceptionOrNull() as PairingException

        assertEquals("pairing_timeout", failure.code)
        assertNotNull(stored)
        assertFalse(deleted)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun explicitCommitRejectionDeletesCandidate() = runTest {
        var stored: ControllerGrant? = null
        var deleted = false
        val channel = MockDeviceChannel(
            candidateStored = { stored != null },
            commitError = "auth_failed",
        )
        val failure = runCatching {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null; deleted = true },
            )
        }.exceptionOrNull() as PairingException

        assertEquals("auth_failed", failure.code)
        assertNull(stored)
        assertTrue(deleted)
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test fun pendingExpiryTimesOutWithoutCreatingCredential() = runTest {
        var stored: ControllerGrant? = null
        val channel = MockDeviceChannel(candidateStored = { false }, withholdGrant = true)

        val failure = runCatching {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = { stored = it },
                deleteCandidate = { stored = null },
            )
        }.exceptionOrNull() as PairingException

        assertEquals("pairing_timeout", failure.code)
        assertNull(stored)
    }

    @Test fun exactFirmwarePairingErrorIsSurfaced() = runTest {
        val channel = MockDeviceChannel(candidateStored = { false }, controlError = "controller_full")
        val failure = runCatching {
            ControllerPairingProtocol().pair(
                label = "Owner phone",
                channel = channel,
                persistCandidate = {},
                deleteCandidate = {},
            )
        }.exceptionOrNull() as PairingException
        assertEquals("controller_full", failure.code)
    }

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }

    private class MockDeviceChannel(
        private val candidateStored: () -> Boolean,
        private val corruptOK: Boolean = false,
        private val withholdOK: Boolean = false,
        private val withholdGrant: Boolean = false,
        private val controlError: String? = null,
        private val commitError: String? = null,
    ) : PairingChannel {
        private val json = Json
        private val replies = Channel<ByteArray>(Channel.UNLIMITED)
        private val root = ByteArray(32) { (it + 64).toByte() }
        private val controllerId = ByteArray(16) { (it + 16).toByte() }
        private val deviceNonce = ByteArray(16) { (it + 48).toByte() }
        private val deviceUid = "KT1234"
        private lateinit var clientNonce: ByteArray
        val commitSeen = CompletableDeferred<Unit>()
        var commitVerified = false
            private set

        override suspend fun send(payload: ByteArray) {
            val objectValue = json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
            when (objectValue.getValue("type").jsonPrimitive.content) {
                "pair_request" -> {
                    if (controlError != null) {
                        replies.send(
                            buildJsonObject {
                                put("v", 1)
                                put("type", "error")
                                put("code", controlError)
                            }.toString().toByteArray(),
                        )
                        return
                    }
                    clientNonce = SecureEnvelopeSession.decodeUrl(
                        objectValue.getValue("client_nonce_b64").jsonPrimitive.content,
                        "test",
                    )
                    replies.send(
                        buildJsonObject {
                            put("v", 1)
                            put("type", "pair_pending")
                            put("device_nonce_b64", SecureEnvelopeSession.encodeUrl(deviceNonce))
                            put("expires_in_ms", if (withholdGrant) 1 else 60_000)
                        }.toString().toByteArray(),
                    )
                    if (!withholdGrant) {
                        val proof = ControllerPairingProtocol.proof(
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
                                put("proof_b64", SecureEnvelopeSession.encodeUrl(proof))
                            }.toString().toByteArray(),
                        )
                    }
                }
                "pair_commit" -> {
                    assertTrue("candidate must be stored before pair_commit", candidateStored())
                    val actual = SecureEnvelopeSession.decodeUrl(
                        objectValue.getValue("proof_b64").jsonPrimitive.content,
                        "test",
                    )
                    val expected = ControllerPairingProtocol.proof(
                        root,
                        ControllerPairingProtocol.CLIENT_PREFIX,
                        controllerId,
                        deviceUid,
                        clientNonce,
                        deviceNonce,
                    )
                    commitVerified = MessageDigest.isEqual(actual, expected)
                    assertTrue(commitVerified)
                    commitSeen.complete(Unit)
                    if (commitError != null) {
                        replies.send(
                            buildJsonObject {
                                put("v", 1)
                                put("type", "error")
                                put("code", commitError)
                            }.toString().toByteArray(),
                        )
                    } else if (!withholdOK) {
                        val ok = if (corruptOK) ByteArray(32) else ControllerPairingProtocol.proof(
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
                                put("proof_b64", SecureEnvelopeSession.encodeUrl(ok))
                            }.toString().toByteArray(),
                        )
                    }
                }
            }
        }

        override suspend fun receive(): ByteArray = replies.receive()
    }
}
