package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareBlePayloadMapperTest {
    @Test fun acceptsOnlyTheExactAuthenticatedRefreshEvent() {
        val valid = FirmwareBlePayloadMapper.event(
            "companion.refresh",
            """{"v":1,"cursor":"ble:7","kind":"refresh","body":{}}""".toByteArray(),
        )
        assertEquals("ble:7", valid.cursor)
        listOf(
            "status.event" to """{"v":1,"cursor":"ble:7","kind":"refresh","body":{}}""",
            "companion.refresh" to """{"v":1,"cursor":"ble:0","kind":"refresh","body":{}}""",
            "companion.refresh" to """{"v":1,"cursor":"ble:7","kind":"refresh","body":{"extra":true}}""",
            "companion.refresh" to """{"v":1,"cursor":"ble:7","kind":"refresh","body":{},"extra":true}""",
        ).forEach { (operation, payload) ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.event(operation, payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_event", failure.code)
        }
    }

    @Test fun surfacesProductionActionRejection() {
        val actionId = "00000000-0000-4000-8000-000000000001"
        val failure = runCatching {
            FirmwareBlePayloadMapper.action(
                """{"action_id":"$actionId","accepted":false,"state":"rejected","error_code":"companion_unavailable"}""".toByteArray(),
                ActionCommand(ActionKind.PET, actionId),
            )
        }.exceptionOrNull() as TransportException
        assertEquals("companion_unavailable", failure.code)
    }

    @Test fun mapsExactFirmwareStatePayload() {
        val status = FirmwareBlePayloadMapper.state(
            """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","firmware_version":"0.12.0","listening":true,"mood":"content","battery_percent":82,"battery_mv":3980,"pack_ready":true,"pack_id":305419896,"pack_revision":4294967295,"bond_level":3,"bond_xp":91,"bond_progress_percent":45,"evolution_stage":"kit","appearance_variant":2,"personality":"curious","unlock_mask":7,"memory_count":4,"energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_tx_unlocked":false,"mesh_time_valid":true,"mesh_one_shot_ready":true,"event_count":7}""".toByteArray(),
            1_787_000_000,
        )
        assertEquals("KTDEAD", status.deviceId)
        assertEquals("FOX", status.companionName)
        assertEquals(88, status.needs.energy)
        assertTrue(status.mesh.rxReady)
        assertTrue(status.mesh.enabled)
        assertTrue(status.mesh.timeValid)
        assertTrue(status.mesh.oneShotReady)
        assertEquals("0.12.0", status.firmwareVersion)
        assertEquals(82, status.batteryPercent)
        assertTrue(status.packReady)
        assertEquals("305419896", status.packId)
        assertEquals(4_294_967_295L, status.packRevision)
        assertEquals("2", status.appearanceVariant)
        assertEquals(3, status.bondLevel)
        assertEquals("curious", status.personality)
        assertEquals("7", status.cursor)
    }

    @Test fun mapsExactDiscoveryPeerAndMessagePayloads() {
        val history = FirmwareBlePayloadMapper.history(
            """{"schema":"kitsu.history.v1","items":[{"sequence":"9","public_key_b64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","sender_advert_timestamp":100,"observed":{"epoch_valid":true,"epoch":1787000001,"boot_id":"4","millis":2000},"last_hop":{"valid":true,"rssi":-91.5,"snr":7.25}}],"cursor":"9","has_more":false,"gap":true}""".toByteArray(),
        )
        assertEquals("9", history.items.single().id)
        assertTrue(history.items.single().summary.contains("last hop"))
        assertTrue(history.cursorExpired)

        val peers = FirmwareBlePayloadMapper.peers(
            """{"schema":"kitsu.peers.v1","items":[{"public_key_b64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","name":"Home repeater","type":2,"kitsu_named":false,"has_location":false,"lat_e6":0,"lon_e6":0,"sender_advert_timestamp":100,"last_observed":{"epoch_valid":false,"epoch":0,"boot_id":"4","millis":2000},"last_hop":{"valid":true,"rssi":-80.0,"snr":9.0},"last_sequence":"9","sighting_count":2}],"cursor":"9","has_more":false,"gap":false}""".toByteArray(),
        )
        assertEquals("repeater", peers.items.single().role)
        assertNull(peers.items.single().lastHeardAt)

        val messages = FirmwareBlePayloadMapper.messages(
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"11","timestamp":1787000002,"inbound":true,"kind":"channel","authenticated":false,"sender_name":"Alice","peer_id":null,"channel_slot":0,"text":"hello","state":"received"}],"cursor":"11","has_more":false,"gap":false}""".toByteArray(),
        )
        assertEquals("Alice (unverified)", messages.items.single().peerId)
        assertEquals("0", messages.items.single().channel)
    }

    @Test fun rejectsCrossShapedOrUnknownStateMessages() {
        val peer = "A".repeat(43)
        listOf(
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"direct","authenticated":true,"sender_name":"A","peer_id":"$peer","channel_slot":0,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"channel","authenticated":false,"sender_name":"A","peer_id":"$peer","channel_slot":0,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"direct","authenticated":true,"sender_name":"A","peer_id":"$peer","channel_slot":null,"text":"hi","state":"accepted"}],"cursor":"1","has_more":false,"gap":false}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }
    }

    @Test fun mapsConfiguredChannelsAndPinnedMeshProfile() {
        val channels = FirmwareBlePayloadMapper.channels(
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":true,"name":"Ops"},{"slot":3,"configured":false,"name":null}]}""".toByteArray(),
        )
        assertEquals(4, channels.size)
        assertTrue(channels.first().configured == true)
        assertEquals("Public", channels.first().name)

        val receipt = FirmwareBlePayloadMapper.meshConfiguration(
            """{"schema":"kitsu.mesh-config.v1","enabled":true,"profile":"uk_eu_narrow","tx_power_dbm":22}""".toByteArray(),
        )
        assertTrue(receipt.enabled)
        assertEquals("uk_eu_narrow", receipt.profile)
        assertEquals(22, receipt.txPowerDbm)
    }

    @Test fun malformedChannelCatalogFailsClosed() {
        listOf(
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null}]}""",
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":"stale"},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":null},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":true,"name":"bad\u0085name"},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.channels(payload.toByteArray())
            }.exceptionOrNull()
            assertEquals("malformed_channels", (failure as TransportException).code)
        }
    }

    @Test fun validatesControllerForgetAndFirmwareReceipts() {
        assertTrue(FirmwareBlePayloadMapper.controllerForget(
            """{"schema":"kitsu.controller-forget.v1","accepted":true}""".toByteArray(),
        ).accepted)
        val storageFailure = runCatching {
            FirmwareBlePayloadMapper.controllerForget(
                """{"schema":"kitsu.controller-forget.v1","accepted":false,"error":"storage_failed"}""".toByteArray(),
            )
        }.exceptionOrNull() as TransportException
        assertEquals("storage_failed", storageFailure.code)
        listOf(
            """{"schema":"kitsu.controller-forget.v1","accepted":true,"extra":true}""",
            """{"schema":"kitsu.controller-forget.v1","accepted":false,"error":"unknown"}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.controllerForget(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_controller_forget", failure.code)
        }
        val update = FirmwareBlePayloadMapper.firmwareUpdate(
            """{"ok":true,"protocol":1,"state":"receiving","update_id":"${"a".repeat(64)}","firmware_version":"0.11.4","image_bytes":1024,"next_offset":256,"chunk_bytes":4096,"resumed":false,"replayed":false,"scheduled":false,"error":null}""".toByteArray(),
        )
        assertEquals(256, update.nextOffset)

        listOf(
            """{"ok":false,"protocol":1,"state":"failed","update_id":null,"firmware_version":"0.11.4","image_bytes":0,"next_offset":0,"chunk_bytes":4096,"resumed":false,"replayed":false,"scheduled":false,"error":null}""",
            """{"ok":true,"protocol":1,"state":"idle","update_id":null,"firmware_version":"0.11.4","image_bytes":0,"next_offset":0,"chunk_bytes":4096,"resumed":false,"replayed":false,"scheduled":false,"error":null,"extra":true}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.firmwareUpdate(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_firmware_update_receipt", failure.code)
        }
    }

    @Test fun recognizesSignedFirmwareRejection() {
        assertEquals(
            "request_rejected",
            FirmwareBlePayloadMapper.rejectionCode(
                """{"ok":false,"error":"request_rejected"}""".toByteArray(),
            ),
        )
    }
}
