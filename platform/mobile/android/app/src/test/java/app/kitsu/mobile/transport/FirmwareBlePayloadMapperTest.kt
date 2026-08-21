package app.kitsu.mobile.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FirmwareBlePayloadMapperTest {
    @Test fun mapsExactFirmwareStatePayload() {
        val status = FirmwareBlePayloadMapper.state(
            """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_tx_unlocked":false,"mesh_time_valid":true,"mesh_one_shot_ready":true,"wifi_configured":true,"wifi_state":"connected","gateway_configured":true,"gateway_enrolled":false,"lan_state":"connected","journal_ready":true,"peer_count":2,"event_count":7,"controller_count":1,"remote_connectivity_allowed":false}""".toByteArray(),
            1_787_000_000,
        )
        assertEquals("KTDEAD", status.deviceId)
        assertEquals("FOX", status.companionName)
        assertEquals(88, status.needs.energy)
        assertTrue(status.mesh.rxReady)
        assertTrue(status.mesh.enabled)
        assertTrue(status.mesh.timeValid)
        assertTrue(status.mesh.oneShotReady)
        assertTrue(status.mesh.txReady)
        assertTrue(status.lan.wifiConfigured == true)
        assertEquals("connected", status.lan.wifiState)
        assertTrue(status.lan.gatewayConfigured == true)
        assertFalse(status.lan.gatewayEnrolled == true)
        assertEquals("connected", status.lan.lanState)
        assertFalse(status.lan.remoteConnectivityAllowed == true)
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
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"11","timestamp":1787000002,"inbound":true,"kind":"channel","authenticated":false,"sender_name":"Alice","peer_id":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","channel_slot":0,"text":"hello","state":"received"}],"cursor":"11","has_more":false,"gap":false}""".toByteArray(),
        )
        assertEquals("Alice (unverified)", messages.items.single().peerId)
        assertEquals("0", messages.items.single().channel)
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

    @Test fun validatesExactProvisioningReceiptSchemas() {
        val wifi = FirmwareBlePayloadMapper.wifiConfiguration(
            """{"schema":"kitsu.wifi-config.v1","accepted":true,"state":"stored","error_code":null}""".toByteArray(),
        )
        assertTrue(wifi.accepted)
        assertEquals("stored", wifi.state)

        val retry = FirmwareBlePayloadMapper.wifiRetry(
            """{"schema":"kitsu.wifi-retry.v1","accepted":true,"state":"retrying","error_code":null}""".toByteArray(),
        )
        assertTrue(retry.accepted)
        assertEquals("retrying", retry.state)

        val gateway = FirmwareBlePayloadMapper.gatewayConfiguration(
            """{"schema":"kitsu.gateway-config.v2","accepted":true,"state":"stored","error_code":null}""".toByteArray(),
        )
        assertTrue(gateway.accepted)
        assertEquals("stored", gateway.state)
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
