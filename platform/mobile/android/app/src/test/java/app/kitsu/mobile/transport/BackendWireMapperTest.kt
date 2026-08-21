package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.MessageRoute
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BackendWireMapperTest {
    private val json = Json { ignoreUnknownKeys = true }

    @Test fun companionSelectionIsAutomaticOnlyForExactlyOne() {
        val first = companion("00000000-0000-0000-0000-000000000001", "First")
        val second = companion("00000000-0000-0000-0000-000000000002", "Second")

        assertEquals(first, BackendWireMapper.chooseCompanion(listOf(first), null))
        assertNull(BackendWireMapper.chooseCompanion(listOf(first, second), null))
        assertEquals(second, BackendWireMapper.chooseCompanion(listOf(first, second), second.id))
    }

    @Test fun remoteCareActionUsesCanonicalBackendBody() {
        val request = BackendWireMapper.remoteAction(
            ActionCommand(
                kind = ActionKind.LISTEN_ONCE,
                clientRequestId = "00000000-0000-0000-0000-000000000001",
                expiresInMs = 30_001,
                durationMs = 5_000,
            ),
        )

        assertEquals("companion.listen_once", request.actionType)
        assertEquals(31, request.expiresInSeconds)
        assertEquals(5_000, request.parameters.getValue("duration_ms").jsonPrimitive.content.toInt())
    }

    @Test fun remoteMessageRenamesTargetAndHistoryFiltersMessageEvents() {
        val request = BackendWireMapper.remoteAction(
            ActionCommand(
                kind = ActionKind.SEND_MESSAGE,
                clientRequestId = "00000000-0000-0000-0000-000000000001",
                targetId = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                text = "hello",
                messageRoute = MessageRoute.DIRECT,
            ),
        )
        assertEquals("message.send", request.actionType)
        assertEquals(
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
            request.parameters.getValue("target").jsonPrimitive.content,
        )
        assertTrue("target_id" !in request.parameters)

        val events = json.decodeFromString<List<BackendEvent>>(
            """[
              {"cursor":"4","event_id":"00000000-0000-0000-0000-000000000004","event_type":"companion.snapshot","body":{},"received_at":"2026-08-17T10:00:00Z"},
              {"cursor":"5","event_id":"00000000-0000-0000-0000-000000000005","event_type":"mesh.message.received","observed_epoch":1786960800,"body":{"direction":"inbound","peer_id":"0000000000000000000000000000000000000000000000000000000000000000","text":"hello"},"received_at":"2026-08-17T10:00:01Z"}
            ]""",
        )
        val messages = BackendWireMapper.messages(events)
        assertEquals(1, messages.items.size)
        assertEquals("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", messages.items.single().peerId)
        assertEquals("5", messages.cursor)
    }

    @Test fun remoteChannelSendAndProjectedEventKeepCanonicalSlot() {
        val request = BackendWireMapper.remoteAction(
            ActionCommand(
                kind = ActionKind.SEND_MESSAGE,
                clientRequestId = "00000000-0000-0000-0000-000000000001",
                targetId = "3",
                text = "ops",
                messageRoute = MessageRoute.CHANNEL,
            ),
        )
        assertEquals("channel", request.parameters.getValue("route").jsonPrimitive.content)
        assertEquals("3", request.parameters.getValue("target").jsonPrimitive.content)

        val events = json.decodeFromString<List<BackendEvent>>(
            """[{
              "cursor":"6","event_id":"00000000-0000-0000-0000-000000000006",
              "event_type":"mesh.message.sent","observed_epoch":1786960801,
              "body":{"direction":"outbound","route":"channel","target":"3","text":"ops"},
              "received_at":"2026-08-17T10:00:02Z"
            }]""",
        )
        val message = BackendWireMapper.messages(events).items.single()
        assertEquals("3", message.channel)
        assertNull(message.peerId)
    }

    @Test fun snapshotMapsCanonicalProjection() {
        val snapshot = json.decodeFromString<BackendSnapshot>(
            """{
              "companion":{"id":"00000000-0000-0000-0000-000000000001","hardware_uid":"kitsu-1","display_name":"Home Kitsu"},
              "vitals":{"battery_percent":81,"needs":{"energy":7,"curiosity":8,"affection":9}},
              "mood":{"state":"CURIOUS"},
              "mesh":{"enabled":true,"rx_ready":true,"tx_ready":false},
              "connectivity":{"online":true,"provenance":"gateway_mtls_device_hmac","gateway_id":"00112233-4455-6677-8899-aabbccddeeff","last_seen_at":"2026-08-17T10:00:00Z"},
              "cursor":"12"
            }""",
        )
        val status = BackendWireMapper.status(snapshot)
        assertEquals("kitsu-1", status.deviceId)
        assertEquals("CURIOUS", status.mood)
        assertEquals(81, status.batteryPercent)
        assertEquals(12, status.cursor?.toInt())
        assertTrue(status.lan.online == true)
        assertEquals("gateway_mtls_device_hmac", status.lan.provenance)
        assertNull(RemoteSnapshotPolicy.validationError(status))
    }

    @Test fun backendPeerKeysAreCanonicalAndLegacyHexIsMigrationOnly() {
        val canonical = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        val legacyHex = "00".repeat(32)
        val peers = BackendWireMapper.peers(
            listOf(
                json.parseToJsonElement(
                    """{"public_key_b64":"$canonical","public_key_hex":"$legacyHex","name":"Alice","role":"client"}""",
                ).jsonObject,
                json.parseToJsonElement(
                    """{"public_key_hex":"${"01".repeat(32)}","name":"Migrated","role":"client"}""",
                ).jsonObject,
            ),
        )
        assertEquals(canonical, peers.items.first().id)
        assertEquals("AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE", peers.items.last().id)
    }

    @Test fun malformedOrMismatchedPeerKeysFailClosed() {
        val failure = runCatching {
            BackendWireMapper.peers(
                listOf(
                    json.parseToJsonElement(
                        """{"public_key_b64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","public_key_hex":"${"01".repeat(32)}"}""",
                    ).jsonObject,
                ),
            )
        }.exceptionOrNull()
        assertEquals("peer_public_key_mismatch", (failure as TransportException).code)

        val ambiguous = runCatching {
            BackendWireMapper.peers(
                listOf(
                    json.parseToJsonElement(
                        """{"public_key":"${"00".repeat(32)}","name":"legacy"}""",
                    ).jsonObject,
                ),
            )
        }.exceptionOrNull()
        assertEquals("missing_peer_public_key", (ambiguous as TransportException).code)
    }

    @Test fun remoteChannelsPreserveUnknownInsteadOfInventingConfiguration() {
        val channels = BackendWireMapper.channels(
            listOf(
                BackendChannel(0, null, null, 128),
                BackendChannel(1, true, "Ops", 128),
                BackendChannel(2, false, null, 128),
                BackendChannel(3, null, null, 128),
            ),
        )
        assertNull(channels.first().configured)
        assertNull(channels.first().name)
        assertTrue(channels[1].configured == true)
        val failure = runCatching {
            BackendWireMapper.channels(
                listOf(
                    BackendChannel(0, true, "Public", 256),
                    BackendChannel(1, false, null, 128),
                    BackendChannel(2, null, null, 128),
                    BackendChannel(3, null, null, 128),
                ),
            )
        }.exceptionOrNull()
        assertEquals("malformed_channels", (failure as TransportException).code)

        listOf(
            BackendChannel(0, true, null, 128),
            BackendChannel(0, false, "stale", 128),
        ).forEach { invalid ->
            val items = listOf(
                invalid,
                BackendChannel(1, false, null, 128),
                BackendChannel(2, null, null, 128),
                BackendChannel(3, null, null, 128),
            )
            val invalidFailure = runCatching { BackendWireMapper.channels(items) }.exceptionOrNull()
            assertEquals("malformed_channels", (invalidFailure as TransportException).code)
        }
    }

    private fun companion(id: String, name: String) = RemoteCompanion(
        id = id,
        hardwareUid = "hw-$name",
        displayName = name,
        status = "active",
    )
}
