package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.ChannelRegionScope
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
            """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","firmware_version":"0.12.0","listening":true,"mood":"content","battery_percent":82,"battery_mv":3980,"pack_ready":true,"pack_id":305419896,"pack_revision":4294967295,"bond_level":3,"bond_xp":91,"bond_progress_percent":45,"evolution_stage":"kit","appearance_variant":2,"personality":"curious","unlock_mask":7,"memory_count":4,"energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_tx_unlocked":false,"mesh_time_valid":true,"mesh_one_shot_ready":true,"mesh_identity_ready":true,"mesh_advertise_ready":true,"mesh_advertise_retry_after_ms":0,"mesh_advertise_error":null,"event_count":7}""".toByteArray(),
            1_787_000_000,
        )
        assertEquals("KTDEAD", status.deviceId)
        assertEquals("FOX", status.companionName)
        assertEquals(88, status.needs.energy)
        assertTrue(status.mesh.rxReady)
        assertTrue(status.mesh.enabled)
        assertTrue(status.mesh.timeValid)
        assertTrue(status.mesh.oneShotReady)
        assertTrue(status.mesh.advertiseSupported)
        assertTrue(status.mesh.identityReady)
        assertTrue(status.mesh.advertiseReady)
        assertEquals(0L, status.mesh.advertiseRetryAfterMs)
        assertNull(status.mesh.advertiseError)
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

    @Test fun mapsAdvertiseCooldownAndRequiresQueuedReceipt() {
        val status = FirmwareBlePayloadMapper.state(
            """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_time_valid":true,"mesh_identity_ready":true,"mesh_advertise_ready":false,"mesh_advertise_retry_after_ms":29999,"mesh_advertise_error":"advertise_cooldown"}""".toByteArray(),
            1_787_000_000,
        )
        assertTrue(status.mesh.advertiseSupported)
        assertEquals(29_999L, status.mesh.advertiseRetryAfterMs)
        assertEquals("advertise_cooldown", status.mesh.advertiseError)

        val actionId = "00000000-0000-4000-8000-000000000004"
        val command = ActionCommand(
            ActionKind.ADVERTISE_ONCE,
            actionId,
            advertiseScope = AdvertiseScope.NEARBY,
        )
        assertEquals(
            "queued",
            FirmwareBlePayloadMapper.action(
                """{"action_id":"$actionId","accepted":true,"state":"queued"}""".toByteArray(),
                command,
            ).state,
        )
        val malformed = runCatching {
            FirmwareBlePayloadMapper.action(
                """{"action_id":"$actionId","accepted":true,"state":"applied"}""".toByteArray(),
                command,
            )
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_action_receipt", malformed.code)
        val cooldown = runCatching {
            FirmwareBlePayloadMapper.action(
                """{"action_id":"$actionId","accepted":false,"state":"rejected","error_code":"advertise_cooldown"}""".toByteArray(),
                command,
            )
        }.exceptionOrNull() as TransportException
        assertEquals("advertise_cooldown", cooldown.code)
    }

    @Test fun acceptsLegacyStateWithoutAdvertiseCapabilityFields() {
        val status = FirmwareBlePayloadMapper.state(
            """{"schema":"kitsu.state.v1","device_uid":"KTLEGACY","companion":"FOX","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_time_valid":true}""".toByteArray(),
            1_787_000_000,
        )

        assertFalse(status.mesh.advertiseSupported)
        assertFalse(status.mesh.identityReady)
        assertFalse(status.mesh.advertiseReady)
        assertEquals(0L, status.mesh.advertiseRetryAfterMs)
        assertNull(status.mesh.advertiseError)
        assertNull(status.mesh.lastFloodAdvert)
    }

    @Test fun mapsEveryValidLastFloodAdvertStateAndKeepsOuterStateForwardCompatible() {
        val cases = listOf(
            """{"emitted_at":1704067200,"state":"queued","repeat_count":null,"observation_open":false}""" to
                Triple("queued", null, false),
            """{"emitted_at":1787000000,"state":"sent","repeat_count":0,"observation_open":true}""" to
                Triple("sent", 0, true),
            """{"emitted_at":4102444800,"state":"sent","repeat_count":255,"observation_open":false}""" to
                Triple("sent", 255, false),
            """{"emitted_at":1787000000,"state":"tx_failed","repeat_count":null,"observation_open":false}""" to
                Triple("tx_failed", null, false),
        )

        cases.forEachIndexed { index, (cluster, expected) ->
            val status = FirmwareBlePayloadMapper.state(
                stateWithLastFloodAdvert(
                    cluster,
                    extraTopLevel = if (index == 0) ",\"future_state_field\":{\"ignored\":true}" else "",
                ),
                1_787_000_000,
            )
            val actual = requireNotNull(status.mesh.lastFloodAdvert)
            assertEquals(expected.first, actual.state)
            assertEquals(expected.second, actual.repeatCount)
            assertEquals(expected.third, actual.observationOpen)
        }

        val explicitlyAbsent = FirmwareBlePayloadMapper.state(
            stateWithLastFloodAdvert("null"),
            1_787_000_000,
        )
        assertNull(explicitlyAbsent.mesh.lastFloodAdvert)
    }

    @Test fun lastFloodAdvertIsAnExactAtomicStateCluster() {
        val invalidClusters = listOf(
            // Every key, including an explicitly nullable repeat_count, is required.
            """{"emitted_at":1787000000,"state":"queued","observation_open":false}""",
            """{"emitted_at":1787000000,"state":"queued","repeat_count":null,"observation_open":false,"extra":true}""",
            """{"emitted_at":1704067199,"state":"queued","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":4102444801,"state":"queued","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"unknown","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"queued","repeat_count":0,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"queued","repeat_count":null,"observation_open":true}""",
            """{"emitted_at":1787000000,"state":"sent","repeat_count":null,"observation_open":true}""",
            """{"emitted_at":1787000000,"state":"sent","repeat_count":-1,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"sent","repeat_count":256,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"tx_failed","repeat_count":1,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"tx_failed","repeat_count":null,"observation_open":true}""",
        )

        invalidClusters.forEach { cluster ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.state(stateWithLastFloodAdvert(cluster), 1_787_000_000)
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_state", failure.code)
        }
    }

    @Test fun mapsEveryValidNearbyAdvertStateWithoutTouchingFloodEvidence() {
        listOf("queued", "sent", "tx_failed").forEachIndexed { index, state ->
            val status = FirmwareBlePayloadMapper.state(
                stateWithAdvertRecords(
                    nearby = """{"emitted_at":178700000$index,"state":"$state","repeat_count":null,"observation_open":false}""",
                    flood = """{"emitted_at":1787000100,"state":"sent","repeat_count":3,"observation_open":false}""",
                    extraTopLevel = if (index == 0) ",\"future_state_field\":true" else "",
                ),
                1_787_000_000,
            )

            assertEquals(state, requireNotNull(status.mesh.lastNearbyAdvert).state)
            assertEquals(3, requireNotNull(status.mesh.lastFloodAdvert).repeatCount)
        }

        val absent = FirmwareBlePayloadMapper.state(
            stateWithAdvertRecords(nearby = "null", flood = "null"),
            1_787_000_000,
        )
        assertNull(absent.mesh.lastNearbyAdvert)
        assertNull(absent.mesh.lastFloodAdvert)
    }

    @Test fun nearbyAdvertIsAnExactZeroHopAtomicStateCluster() {
        listOf(
            // All four keys, including the explicit null repeat count, are required.
            """{"emitted_at":1787000000,"state":"sent","observation_open":false}""",
            """{"emitted_at":1787000000,"state":"sent","repeat_count":null,"observation_open":false,"extra":true}""",
            """{"emitted_at":1704067199,"state":"sent","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":4102444801,"state":"sent","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"unknown","repeat_count":null,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"queued","repeat_count":0,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"sent","repeat_count":1,"observation_open":false}""",
            """{"emitted_at":1787000000,"state":"tx_failed","repeat_count":null,"observation_open":true}""",
        ).forEach { nearby ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.state(
                    stateWithAdvertRecords(nearby = nearby, flood = "null"),
                    1_787_000_000,
                )
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_state", failure.code)
        }
    }

    @Test fun authenticatedZeroSixteenOnePrefersStrictFloodAdvertV2Sources() {
        val enhanced = """{"emitted_at":1787000200,"state":"sent","repeat_count":2,"observation_open":true,"repeat_sources":[{"last_hop_token":"00"}],"repeat_sources_truncated":false}"""
        val legacy = """{"emitted_at":1787000200,"state":"sent","repeat_count":2,"observation_open":true}"""
        val status = FirmwareBlePayloadMapper.state(
            stateWithFloodAdvertV2("0.16.1", enhanced, legacy),
            1_787_000_000,
        )
        val mapped = requireNotNull(status.mesh.lastFloodAdvert)
        assertEquals(2, mapped.repeatCount)
        assertEquals("00", requireNotNull(mapped.repeatSources).single().lastHopToken)
        assertEquals(false, mapped.repeatSourcesTruncated)

        val queued = FirmwareBlePayloadMapper.state(
            stateWithFloodAdvertV2(
                "0.16.1-test",
                """{"emitted_at":1787000201,"state":"queued","repeat_count":null,"observation_open":false,"repeat_sources":null,"repeat_sources_truncated":null}""",
                """{"emitted_at":1787000201,"state":"queued","repeat_count":null,"observation_open":false}""",
            ),
            1_787_000_000,
        )
        assertNull(requireNotNull(queued.mesh.lastFloodAdvert).repeatSources)
    }

    @Test fun preZeroSixteenOneIgnoresFloodAdvertV2AndKeepsLegacyCompatibility() {
        val legacy = """{"emitted_at":1787000200,"state":"sent","repeat_count":4,"observation_open":false}"""
        val status = FirmwareBlePayloadMapper.state(
            // This intentionally malformed future cluster is ignored by 0.16.0.
            stateWithFloodAdvertV2("0.16.0", """{"future":true}""", legacy),
            1_787_000_000,
        )

        assertEquals(4, status.mesh.lastFloodAdvert?.repeatCount)
        assertNull(status.mesh.lastFloodAdvert?.repeatSources)
    }

    @Test fun floodAdvertV2IsAnExactBoundedSourceCluster() {
        val legacy = """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true}"""
        listOf(
            // Both new keys are required even when explicitly null.
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[]}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[],"repeat_sources_truncated":false,"extra":true}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":null,"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[{"last_hop_token":"aa"}],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":2,"observation_open":true,"repeat_sources":[{"last_hop_token":"AA"},{"last_hop_token":"AA"}],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[{"last_hop_token":"AA","extra":true}],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[{"last_hop_token":"AA"}],"repeat_sources_truncated":true}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":0,"observation_open":true,"repeat_sources":[{"last_hop_token":"AA"}],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[{"last_hop_token":"AA"},{"last_hop_token":"BB"}],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"sent","repeat_count":4,"observation_open":true,"repeat_sources":[{"last_hop_token":"01"},{"last_hop_token":"02"},{"last_hop_token":"03"},{"last_hop_token":"04"}],"repeat_sources_truncated":true}""",
            """{"emitted_at":1787000200,"state":"queued","repeat_count":null,"observation_open":false,"repeat_sources":[],"repeat_sources_truncated":false}""",
            """{"emitted_at":1787000200,"state":"tx_failed","repeat_count":null,"observation_open":false,"repeat_sources":null,"repeat_sources_truncated":false}""",
        ).forEach { enhanced ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.state(
                    stateWithFloodAdvertV2("0.16.1", enhanced, legacy),
                    1_787_000_000,
                )
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_state", failure.code)
        }
    }

    @Test fun floodAdvertV2MustBePresentAndCoreBoundToLegacyOnZeroSixteenOne() {
        val legacy = """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true}"""
        val enhanced = """{"emitted_at":1787000200,"state":"sent","repeat_count":1,"observation_open":true,"repeat_sources":[{"last_hop_token":"00"}],"repeat_sources_truncated":false}"""

        val bothNull = FirmwareBlePayloadMapper.state(
            stateWithFloodAdvertV2("0.16.1", "null", "null"),
            1_787_000_000,
        )
        assertNull(bothNull.mesh.lastFloodAdvert)

        val missingEnhanced = stateWithFloodAdvertV2("0.16.1", enhanced, legacy)
            .toString(Charsets.UTF_8)
            .replace(",\"mesh_last_flood_advert_v2\":$enhanced", "")
            .toByteArray()
        val missingLegacy = stateWithFloodAdvertV2("0.16.1", enhanced, legacy)
            .toString(Charsets.UTF_8)
            .replace("\"mesh_last_flood_advert\":$legacy,", "")
            .toByteArray()
        val missingNearby = stateWithFloodAdvertV2("0.16.1", enhanced, legacy)
            .toString(Charsets.UTF_8)
            .replace(",\"mesh_last_nearby_advert\":null", "")
            .toByteArray()
        val mismatchedNull = stateWithFloodAdvertV2("0.16.1", "null", legacy)
        val mismatchedCore = stateWithFloodAdvertV2(
            "0.16.1",
            enhanced.replace("\"repeat_count\":1", "\"repeat_count\":2"),
            legacy,
        )

        listOf(missingEnhanced, missingLegacy, missingNearby, mismatchedNull, mismatchedCore).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.state(payload, 1_787_000_000)
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_state", failure.code)
        }
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
        assertNull(messages.items.single().peerId)
        assertEquals("0", messages.items.single().channel)
        assertEquals("Alice", messages.items.single().senderName)
    }

    private fun stateWithLastFloodAdvert(
        cluster: String,
        extraTopLevel: String = "",
    ): ByteArray = (
        """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_time_valid":true,"mesh_last_flood_advert":$cluster$extraTopLevel}"""
    ).toByteArray()

    private fun stateWithAdvertRecords(
        nearby: String,
        flood: String,
        extraTopLevel: String = "",
    ): ByteArray = (
        """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_time_valid":true,"mesh_last_flood_advert":$flood,"mesh_last_nearby_advert":$nearby$extraTopLevel}"""
    ).toByteArray()

    private fun stateWithFloodAdvertV2(
        firmwareVersion: String,
        enhanced: String,
        legacy: String,
    ): ByteArray = (
        """{"schema":"kitsu.state.v1","device_uid":"KTDEAD","companion":"FOX","firmware_version":"$firmwareVersion","energy":88,"curiosity":72,"affection":91,"sleeping":false,"mesh_enabled":true,"mesh_rx_ready":true,"mesh_time_valid":true,"mesh_last_flood_advert":$legacy,"mesh_last_flood_advert_v2":$enhanced,"mesh_last_nearby_advert":null}"""
    ).toByteArray()

    @Test fun mapsFrozenV2DirectDeliveryAndInboundChannelEvidence() {
        val messages = FirmwareBlePayloadMapper.messages(
            """{"schema":"kitsu.messages.v2","journal_session":"4","journal_revision":"10","items":[{"message_id":"7","revision":"9","timestamp":1787000002,"inbound":false,"kind":"direct","peer_id":"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"delivered","route":"flood","local_tx":"sent","delivery_ack":"received","repeater_count":2,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null},{"message_id":"8","revision":"10","timestamp":1787000010,"inbound":true,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":true,"sender_name":"Alice","text":"hi","state":"received","route":"flood","local_tx":"not_applicable","delivery_ack":"not_applicable","repeater_count":1,"repeaters_heard":null,"rssi_dbm":-91.5,"snr_db":7.2}],"cursor":"8","has_more":false,"gap":false}""".toByteArray(),
        )

        assertEquals(2, messages.protocolVersion)
        assertEquals("4", messages.journalSession)
        assertEquals("10", messages.journalRevision)
        val delivered = messages.items[0]
        assertEquals("9", delivered.revision)
        assertEquals("received", delivered.deliveryAck)
        assertEquals(2, delivered.repeaterCount)
        val incoming = messages.items[1]
        assertEquals("Alice", incoming.senderName)
        assertEquals(true, incoming.unreadOnKitsu)
        assertEquals(-91.5, incoming.rssiDbm)
        assertEquals(7.2, incoming.snrDb)
    }

    @Test fun acceptsEveryTruthfulV2OutgoingLifecycleShape() {
        val shapes = listOf(
            Triple("queued", "pending", "not_applicable"),
            Triple("sent", "sent", "pending"),
            Triple("delivered", "sent", "received"),
            Triple("unconfirmed", "sent", "not_received"),
            Triple("failed", "failed", "not_applicable"),
            Triple("cancelled", "cancelled", "not_applicable"),
        )

        shapes.forEachIndexed { index, (state, localTx, ack) ->
            val repeater = if (state == "delivered") "0" else "null"
            val page = FirmwareBlePayloadMapper.messages(
                v2Page(
                    """{"message_id":"${index + 1}","revision":"${index + 1}","timestamp":1,"inbound":false,"kind":"direct","peer_id":"${"A".repeat(43)}","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"$state","route":"direct","local_tx":"$localTx","delivery_ack":"$ack","repeater_count":$repeater,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}""",
                    cursor = (index + 1).toString(),
                    revision = (index + 1).toString(),
                ).toByteArray(),
            )
            assertEquals(state, page.items.single().state)
        }
    }

    @Test fun acceptsDeliveryAckWithoutInventingRouteEvidence() {
        val peer = "A".repeat(43)
        val delivered = FirmwareBlePayloadMapper.messages(
            v2Page(
                """{"message_id":"1","revision":"2","timestamp":1,"inbound":false,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"delivered","route":"flood","local_tx":"sent","delivery_ack":"received","repeater_count":null,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}""",
                revision = "2",
            ).toByteArray(),
        ).items.single()

        assertEquals("delivered", delivered.state)
        assertEquals("received", delivered.deliveryAck)
        assertNull(delivered.repeaterCount)
    }

    @Test fun mapsProductionShapedAuthenticatedDirectInbound() {
        val peer = "A".repeat(43)
        val item = """{"message_id":"12","revision":"14","timestamp":1787000020,"inbound":true,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"Copper Fox","text":"hello direct","state":"received","route":"direct","local_tx":"not_applicable","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"rssi_dbm":-80.0,"snr_db":9.5}"""

        val mapped = FirmwareBlePayloadMapper.messages(
            v2Page(item, cursor = "12", revision = "14").toByteArray(),
        ).items.single()

        assertEquals("inbound", mapped.direction)
        assertEquals(peer, mapped.peerId)
        assertEquals("Copper Fox", mapped.senderName)
        assertNull(mapped.repeaterCount)
        assertEquals("direct", mapped.route)
    }

    @Test fun rejectsV2EvidenceThatWouldOverclaimDeliveryOrRepeaterFanout() {
        val peer = "A".repeat(43)
        listOf(
            // Delivered requires an authenticated receive ACK; route count may be unknown.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"delivered","route":"direct","local_tx":"sent","delivery_ack":"pending","repeater_count":0,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}""",
            // Fan-out receipts do not exist.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"delivered","route":"direct","local_tx":"sent","delivery_ack":"received","repeater_count":0,"repeaters_heard":2,"rssi_dbm":null,"snr_db":null}""",
            // Channel sends never acquire direct-recipient delivery state.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"delivered","route":"flood","local_tx":"sent","delivery_ack":"received","repeater_count":0,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}""",
        ).forEach { item ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(v2Page(item).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }
    }

    @Test fun v2RemainsStrictAndRejectsTheV3RepeatCountField() {
        val item = """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":1,"rssi_dbm":null,"snr_db":null}"""

        val failure = runCatching {
            FirmwareBlePayloadMapper.messages(v2Page(item).toByteArray())
        }.exceptionOrNull() as TransportException

        assertEquals("malformed_messages", failure.code)
    }

    @Test fun authenticatedOperationSelectionIsBoundToTheMatchingResponseSchema() {
        val v2Item = """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}"""
        val v3Item = """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":null,"rssi_dbm":null,"snr_db":null}"""

        assertEquals(
            2,
            FirmwareBlePayloadMapper.messages(
                v2Page(v2Item).toByteArray(),
                expectedProtocolVersion = 2,
            ).protocolVersion,
        )
        assertEquals(
            3,
            FirmwareBlePayloadMapper.messages(
                v3Page(v3Item).toByteArray(),
                expectedProtocolVersion = 3,
            ).protocolVersion,
        )
        listOf(
            v2Page(v2Item).toByteArray() to 3,
            v3Page(v3Item).toByteArray() to 2,
        ).forEach { (payload, expected) ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(payload, expectedProtocolVersion = expected)
            }.exceptionOrNull() as TransportException
            assertEquals("unsupported_protocol", failure.code)
        }
    }

    @Test fun v3RequiresTheRawRepeatCountKeyEvenWhenTheValueWouldBeNull() {
        val itemMissingRepeatCount = """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}"""

        val failure = runCatching {
            FirmwareBlePayloadMapper.messages(v3Page(itemMissingRepeatCount).toByteArray())
        }.exceptionOrNull() as TransportException

        assertEquals("malformed_messages", failure.code)
    }

    @Test fun mapsEveryBoundedV3LocalChannelRepeatObservation() {
        listOf<Int?>(null, 0, 1, 255).forEachIndexed { index, count ->
            val id = (index + 1).toString()
            val repeatCount = count?.toString() ?: "null"
            val item = """{"message_id":"$id","revision":"$id","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":$repeatCount,"rssi_dbm":null,"snr_db":null}"""

            val page = FirmwareBlePayloadMapper.messages(
                v3Page(item, cursor = id, revision = id).toByteArray(),
            )

            assertEquals(3, page.protocolVersion)
            assertEquals(count, page.items.single().repeatCount)
            assertNull(page.items.single().repeaterCount)
        }
    }

    @Test fun v3RepeatCountIsRestrictedToAValidOutboundSentChannelShape() {
        val peer = "A".repeat(43)
        listOf(
            // The field is an unsigned byte-sized observation count.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":-1,"rssi_dbm":null,"snr_db":null}""",
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":256,"rssi_dbm":null,"snr_db":null}""",
            // Direct sends cannot carry a local channel-repeat observation.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"direct","local_tx":"sent","delivery_ack":"pending","repeater_count":null,"repeaters_heard":null,"repeat_count":1,"rssi_dbm":null,"snr_db":null}""",
            // Inbound route evidence remains repeater_count, never repeat_count.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":true,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":true,"sender_name":"Alice","text":"hello","state":"received","route":"flood","local_tx":"not_applicable","delivery_ack":"not_applicable","repeater_count":0,"repeaters_heard":null,"repeat_count":1,"rssi_dbm":-80.0,"snr_db":9.0}""",
            // Queue state has no radio observation yet.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"queued","route":"flood","local_tx":"pending","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":1,"rssi_dbm":null,"snr_db":null}""",
            // A route count and local repeat count cannot be conflated.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":1,"repeaters_heard":null,"repeat_count":1,"rssi_dbm":null,"snr_db":null}""",
            // repeaters_heard remains reserved and null in v3.
            """{"message_id":"1","revision":"1","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"hello","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":1,"repeat_count":1,"rssi_dbm":null,"snr_db":null}""",
        ).forEach { item ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(v3Page(item).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }
    }

    @Test fun v4MapsRequiredObservationLifecycleAndBoundedSourceTokens() {
        val open = FirmwareBlePayloadMapper.messages(
            v4Page(v4SentItem(repeatCount = 1, open = true, sources = """[{"last_hop_token":"A1B2"}]""")).toByteArray(),
            expectedProtocolVersion = 4,
        )
        val item = open.items.single()
        assertEquals(4, open.protocolVersion)
        assertEquals(1, item.repeatCount)
        assertEquals(true, item.repeatObservationOpen)
        assertEquals("A1B2", requireNotNull(item.repeatSources).single().lastHopToken)
        assertEquals(false, item.repeatSourcesTruncated)

        val closedSaturated = FirmwareBlePayloadMapper.messages(
            v4Page(
                v4SentItem(
                    repeatCount = 255,
                    open = false,
                    sources = """[{"last_hop_token":"01"},{"last_hop_token":"0203"},{"last_hop_token":"040506"},{"last_hop_token":"07"}]""",
                    truncated = true,
                ),
            ).toByteArray(),
        ).items.single()
        assertEquals(false, closedSaturated.repeatObservationOpen)
        assertEquals(4, requireNotNull(closedSaturated.repeatSources).size)
        assertEquals(true, closedSaturated.repeatSourcesTruncated)
    }

    @Test fun v4AcceptsOnlyAnAtomicAllNullTrackerUnavailableFallback() {
        val unavailable = v4SentItem(0, true)
            .replace("\"repeat_count\":0", "\"repeat_count\":null")
            .replace("\"repeat_observation_open\":true", "\"repeat_observation_open\":null")
            .replace("\"repeat_sources\":[]", "\"repeat_sources\":null")
            .replace("\"repeat_sources_truncated\":false", "\"repeat_sources_truncated\":null")
        val mapped = FirmwareBlePayloadMapper.messages(v4Page(unavailable).toByteArray()).items.single()
        assertNull(mapped.repeatCount)
        assertNull(mapped.repeatObservationOpen)
        assertNull(mapped.repeatSources)
        assertNull(mapped.repeatSourcesTruncated)

        listOf(
            unavailable.replace("\"repeat_count\":null", "\"repeat_count\":0"),
            unavailable.replace("\"repeat_observation_open\":null", "\"repeat_observation_open\":false"),
            unavailable.replace("\"repeat_sources\":null", "\"repeat_sources\":[]"),
            unavailable.replace("\"repeat_sources_truncated\":null", "\"repeat_sources_truncated\":false"),
        ).forEach { partial ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(v4Page(partial).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }
    }

    @Test fun v4StrictDecoderKeepsATrackerFullPageVisibleAsUnconfirmed() {
        val unavailableItems = (1..8).joinToString(",") { id ->
            v4SentItem(0, true, id = id)
                .replace("\"repeat_count\":0", "\"repeat_count\":null")
                .replace("\"repeat_observation_open\":true", "\"repeat_observation_open\":null")
                .replace("\"repeat_sources\":[]", "\"repeat_sources\":null")
                .replace("\"repeat_sources_truncated\":false", "\"repeat_sources_truncated\":null")
        }
        val page = FirmwareBlePayloadMapper.messages(
            v4Page(unavailableItems, cursor = "8", revision = "8").toByteArray(),
        )

        assertEquals(8, page.items.size)
        assertTrue(page.items.all { it.repeatCount == null && it.repeatObservationOpen == null })
    }

    @Test fun v4RequiresEveryNewKeyWhileV3RejectsEveryV4Extension() {
        val valid = v4SentItem(repeatCount = 0, open = true)
        listOf(
            ",\"repeat_observation_open\":true",
            ",\"repeat_sources\":[]",
            ",\"repeat_sources_truncated\":false",
        ).forEach { field ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(v4Page(valid.replace(field, "")).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }

        val v3Failure = runCatching {
            FirmwareBlePayloadMapper.messages(v3Page(valid).toByteArray())
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_messages", v3Failure.code)
    }

    @Test fun v4RejectsInvalidOrOverclaimingRepeatSourceShapes() {
        val invalidSentItems = listOf(
            v4SentItem(0, true).replace("\"repeat_observation_open\":true", "\"repeat_observation_open\":null"),
            v4SentItem(0, true).replace("\"repeat_sources\":[]", "\"repeat_sources\":null"),
            v4SentItem(0, true).replace("\"repeat_sources_truncated\":false", "\"repeat_sources_truncated\":null"),
            v4SentItem(1, true, """[{"last_hop_token":"a1"}]"""),
            v4SentItem(1, true, """[{"last_hop_token":"A"}]"""),
            v4SentItem(1, true, """[{"last_hop_token":"A1B2C3D4"}]"""),
            v4SentItem(2, true, """[{"last_hop_token":"A1"},{"last_hop_token":"A1"}]"""),
            v4SentItem(1, true, """[{"last_hop_token":"A1","extra":true}]"""),
            v4SentItem(5, true, """[{"last_hop_token":"01"},{"last_hop_token":"02"},{"last_hop_token":"03"},{"last_hop_token":"04"},{"last_hop_token":"05"}]"""),
            v4SentItem(1, true, """[{"last_hop_token":"A1"}]""", truncated = true),
            v4SentItem(0, true, """[{"last_hop_token":"A1"}]"""),
            v4SentItem(1, true, """[{"last_hop_token":"A1"},{"last_hop_token":"B2"}]"""),
            v4SentItem(4, true, """[{"last_hop_token":"01"},{"last_hop_token":"02"},{"last_hop_token":"03"},{"last_hop_token":"04"}]""", truncated = true),
        )
        invalidSentItems.forEach { invalid ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(v4Page(invalid).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }

        val inboundWithEvidence = v4InboundItem().replace(
            "\"repeat_count\":null,\"repeat_observation_open\":null,\"repeat_sources\":null,\"repeat_sources_truncated\":null",
            "\"repeat_count\":1,\"repeat_observation_open\":true,\"repeat_sources\":[{\"last_hop_token\":\"A1\"}],\"repeat_sources_truncated\":false",
        )
        val failure = runCatching {
            FirmwareBlePayloadMapper.messages(v4Page(inboundWithEvidence).toByteArray())
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_messages", failure.code)
    }

    @Test fun v4AcceptsExplicitNullRepeatClusterOnEveryNonSentShape() {
        val mapped = FirmwareBlePayloadMapper.messages(v4Page(v4InboundItem()).toByteArray()).items.single()
        assertNull(mapped.repeatCount)
        assertNull(mapped.repeatObservationOpen)
        assertNull(mapped.repeatSources)
        assertNull(mapped.repeatSourcesTruncated)
    }

    @Test fun v4StrictDecoderAcceptsWorstCaseBoundedPage() {
        val items = (1..100).joinToString(",") { id ->
            v4SentItem(
                repeatCount = 255,
                open = true,
                sources = """[{"last_hop_token":"01"},{"last_hop_token":"0203"},{"last_hop_token":"040506"},{"last_hop_token":"07"}]""",
                truncated = true,
                id = id,
                text = "x".repeat(160),
            )
        }
        val page = FirmwareBlePayloadMapper.messages(
            v4Page(items, cursor = "100", revision = "100").toByteArray(),
            expectedProtocolVersion = 4,
        )

        assertEquals(100, page.items.size)
        assertEquals(4, page.protocolVersion)
        assertEquals(255, page.items.last().repeatCount)
    }

    @Test fun rejectsNonCanonicalV2Uint32Strings() {
        val peer = "A".repeat(43)
        val item = """{"message_id":"01","revision":"1","timestamp":1,"inbound":false,"kind":"direct","peer_id":"$peer","channel_slot":null,"authenticated":true,"unread":false,"sender_name":"","text":"hello","state":"queued","route":"direct","local_tx":"pending","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"rssi_dbm":null,"snr_db":null}"""

        val failure = runCatching {
            FirmwareBlePayloadMapper.messages(v2Page(item, cursor = "01").toByteArray())
        }.exceptionOrNull() as TransportException

        assertEquals("malformed_messages", failure.code)
    }

    @Test fun rejectsCrossShapedOrUnknownStateMessages() {
        val peer = "A".repeat(43)
        listOf(
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"direct","authenticated":true,"sender_name":"A","peer_id":"$peer","channel_slot":0,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"channel","authenticated":false,"sender_name":"A","peer_id":"$peer","channel_slot":0,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"direct","authenticated":true,"sender_name":"A","peer_id":"$peer","channel_slot":null,"text":"hi","state":"accepted"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"direct","authenticated":false,"sender_name":"A","peer_id":"$peer","channel_slot":null,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
            """{"schema":"kitsu.messages.v1","items":[{"message_id":"1","timestamp":1,"inbound":true,"kind":"channel","authenticated":true,"sender_name":"A","peer_id":null,"channel_slot":0,"text":"hi","state":"received"}],"cursor":"1","has_more":false,"gap":false}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messages(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_messages", failure.code)
        }
    }

    @Test fun mapsStrictRequestBoundMessageReadReceipt() {
        val receipt = FirmwareBlePayloadMapper.messageMarkRead(
            """{"schema":"kitsu.messages-mark-read.v1","accepted":true,"error":null,"marked_count":2,"unchanged_count":1,"journal_session":"7","journal_revision":"22"}""".toByteArray(),
        )

        assertTrue(receipt.accepted)
        assertEquals(2, receipt.markedCount)
        assertEquals(1, receipt.unchangedCount)
        assertEquals("7", receipt.journalSession)
    }

    @Test fun acceptsOnlyFrozenMessageReadRejectionVocabulary() {
        listOf(
            "request_rejected",
            "journal_session_mismatch",
            "snapshot_changed",
            "message_not_inbound",
        ).forEach { error ->
            val receipt = FirmwareBlePayloadMapper.messageMarkRead(
                """{"schema":"kitsu.messages-mark-read.v1","accepted":false,"error":"$error","marked_count":0,"unchanged_count":0,"journal_session":"7","journal_revision":"22"}""".toByteArray(),
            )
            assertEquals(error, receipt.error)
        }

        listOf(
            """{"schema":"kitsu.messages-mark-read.v1","accepted":false,"error":"unknown","marked_count":0,"unchanged_count":0,"journal_session":"7","journal_revision":"22"}""",
            """{"schema":"kitsu.messages-mark-read.v1","accepted":true,"error":null,"marked_count":0,"unchanged_count":0,"journal_session":"7","journal_revision":"22"}""",
            """{"schema":"kitsu.messages-mark-read.v1","accepted":false,"error":"snapshot_changed","marked_count":1,"unchanged_count":0,"journal_session":"7","journal_revision":"22"}""",
            """{"schema":"kitsu.messages-mark-read.v1","accepted":true,"error":null,"marked_count":1,"unchanged_count":0,"journal_session":"7","journal_revision":"22","extra":true}""",
        ).forEach { malformed ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.messageMarkRead(malformed.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_message_read", failure.code)
        }
    }

    private fun v2Page(item: String, cursor: String = "1", revision: String = "1") =
        """{"schema":"kitsu.messages.v2","journal_session":"1","journal_revision":"$revision","items":[$item],"cursor":"$cursor","has_more":false,"gap":false}"""

    private fun v3Page(item: String, cursor: String = "1", revision: String = "1") =
        """{"schema":"kitsu.messages.v3","journal_session":"1","journal_revision":"$revision","items":[$item],"cursor":"$cursor","has_more":false,"gap":false}"""

    private fun v4Page(item: String, cursor: String = "1", revision: String = "1") =
        """{"schema":"kitsu.messages.v4","journal_session":"1","journal_revision":"$revision","items":[$item],"cursor":"$cursor","has_more":false,"gap":false}"""

    private fun v4SentItem(
        repeatCount: Int,
        open: Boolean,
        sources: String = "[]",
        truncated: Boolean = false,
        id: Int = 1,
        text: String = "hello",
    ) = """{"message_id":"$id","revision":"$id","timestamp":1,"inbound":false,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":false,"sender_name":"","text":"$text","state":"sent","route":"flood","local_tx":"sent","delivery_ack":"not_applicable","repeater_count":null,"repeaters_heard":null,"repeat_count":$repeatCount,"repeat_observation_open":$open,"repeat_sources":$sources,"repeat_sources_truncated":$truncated,"rssi_dbm":null,"snr_db":null}"""

    private fun v4InboundItem() =
        """{"message_id":"1","revision":"1","timestamp":1,"inbound":true,"kind":"channel","peer_id":null,"channel_slot":0,"authenticated":false,"unread":true,"sender_name":"Alice","text":"hello","state":"received","route":"flood","local_tx":"not_applicable","delivery_ack":"not_applicable","repeater_count":0,"repeaters_heard":null,"repeat_count":null,"repeat_observation_open":null,"repeat_sources":null,"repeat_sources_truncated":null,"rssi_dbm":-80.0,"snr_db":9.0}"""

    @Test fun mapsExactV1AndV2ChannelsAndPinnedMeshProfile() {
        val legacyChannels = FirmwareBlePayloadMapper.channels(
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":true,"name":"Ops"},{"slot":3,"configured":false,"name":null}]}""".toByteArray(),
        )
        assertEquals(4, legacyChannels.size)
        assertTrue(legacyChannels.first().configured == true)
        assertEquals("Public", legacyChannels.first().name)
        assertTrue(legacyChannels.all { it.regionScope == null })

        val scopedChannels = FirmwareBlePayloadMapper.channels(
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":null},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":true,"name":"Ops","region_scope":"EU"},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""".toByteArray(),
            expectedProtocolVersion = 2,
        )
        assertEquals(null, scopedChannels[0].regionScope)
        assertEquals(null, scopedChannels[1].regionScope)
        assertEquals(ChannelRegionScope.EU, scopedChannels[2].regionScope)

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
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public","region_scope":"US"},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public","region_scope":"eu"},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
            """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null,"region_scope":"EU"},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.channels(payload.toByteArray())
            }.exceptionOrNull()
            assertEquals("malformed_channels", (failure as TransportException).code)
        }
    }

    @Test fun v2ChannelCatalogRequiresExactScopeKeysAndValues() {
        listOf(
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":"US"},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":"eu"},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":"EU"},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":false,"name":null,"region_scope":null},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"General","region_scope":null},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
            """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":null},{"slot":1,"configured":false,"name":null,"region_scope":"EU"},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}""",
        ).forEach { payload ->
            val failure = runCatching {
                FirmwareBlePayloadMapper.channels(payload.toByteArray(), expectedProtocolVersion = 2)
            }.exceptionOrNull()
            assertEquals("malformed_channels", (failure as TransportException).code)
        }
    }

    @Test fun channelResponseSchemaMustMatchTheSelectedOperationVersion() {
        val v1 = """{"schema":"kitsu.channels.v1","items":[{"slot":0,"configured":true,"name":"Public"},{"slot":1,"configured":false,"name":null},{"slot":2,"configured":false,"name":null},{"slot":3,"configured":false,"name":null}]}"""
        val v2 = """{"schema":"kitsu.channels.v2","items":[{"slot":0,"configured":true,"name":"Public","region_scope":null},{"slot":1,"configured":false,"name":null,"region_scope":null},{"slot":2,"configured":false,"name":null,"region_scope":null},{"slot":3,"configured":false,"name":null,"region_scope":null}]}"""

        assertEquals(
            "unsupported_protocol",
            (runCatching {
                FirmwareBlePayloadMapper.channels(v1.toByteArray(), expectedProtocolVersion = 2)
            }.exceptionOrNull() as TransportException).code,
        )
        assertEquals(
            "unsupported_protocol",
            (runCatching {
                FirmwareBlePayloadMapper.channels(v2.toByteArray(), expectedProtocolVersion = 1)
            }.exceptionOrNull() as TransportException).code,
        )
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
