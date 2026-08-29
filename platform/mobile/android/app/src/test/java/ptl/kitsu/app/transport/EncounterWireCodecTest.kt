package ptl.kitsu.app.transport

import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterCatalogPage
import ptl.kitsu.app.model.EncounterDiscoveryPage
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG

class EncounterWireCodecTest {
    private val exactJson = Json { encodeDefaults = true; explicitNulls = true }

    @Test
    fun catalogAcceptsOnlyTheExactTwentyOneCreaturePublicRoster() {
        val encoded = Json.encodeToString(
            EncounterCatalogPage(
                schema = "kitsu.encounter-catalog.v1",
                items = PUBLIC_ENCOUNTER_CATALOG.reversed(),
            ),
        )
        assertTrue("\"creature_name\":\"Frog\"" in encoded)

        val page = EncounterWireCodec.catalog(encoded.toByteArray())
        assertEquals(PUBLIC_ENCOUNTER_CATALOG, page.items)

        listOf(
            EncounterCatalogPage("wrong", PUBLIC_ENCOUNTER_CATALOG),
            EncounterCatalogPage("kitsu.encounter-catalog.v1", PUBLIC_ENCOUNTER_CATALOG.dropLast(1)),
            EncounterCatalogPage(
                "kitsu.encounter-catalog.v1",
                PUBLIC_ENCOUNTER_CATALOG.dropLast(1) + PUBLIC_ENCOUNTER_CATALOG.first().copy(
                    packId = 1,
                ),
            ),
        ).forEach { invalid ->
            val failure = runCatching {
                EncounterWireCodec.catalog(Json.encodeToString(invalid).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_encounter_catalog", failure.code)
        }
    }

    @Test
    fun codePageAcceptsForwardFieldsAndNormalizesInstalledState() {
        val page = EncounterWireCodec.codePage(
            """{
                "schema":"kitsu.encounter-codes.v1",
                "items":[{
                    "device_id":"KT12AF",
                    "code_id":"event:7",
                    "code":"K8-ABCDE-FGHJK-MNPQR",
                    "pack_id":4294967295,
                    "rarity":"mythical",
                    "source":"mesh_repeater",
                    "acquired_at_epoch":1787600000,
                    "installed":true,
                    "future_field":"ignored"
                }],
                "cursor":"generation:7",
                "has_more":false,
                "future_page_field":true
            }""".trimIndent().toByteArray(),
        )

        val code = page.items.single()
        assertEquals(EncounterRarity.MYTHICAL, code.rarity)
        assertEquals(4_294_967_295L, code.packId)
        assertTrue(code.installed)
        assertTrue(code.redeemed)
    }

    @Test
    fun discoveryAcceptsExactTwentyOnePackUnsignedCountsAndSources() {
        val records = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            EncounterDiscoveryRecord(creature.packId, 0, null)
        }.map { record ->
            if (record.packId == 0xF0F750BDL) record.copy(
                encounterCount = 65_535,
                lastSource = "mesh_advert_rx",
            ) else record
        }
        val encoded = exactJson.encodeToString(
            EncounterDiscoveryPage("kitsu.encounter-discovery.v1", records),
        )

        val decoded = EncounterWireCodec.discovery(encoded.toByteArray())

        assertEquals(21, decoded.items.size)
        assertEquals(0xF0F750BDL, decoded.items.single { it.encounterCount > 0 }.packId)
        assertEquals(65_535, decoded.items.single { it.encounterCount > 0 }.encounterCount)
        assertEquals("mesh_advert_rx", decoded.items.single { it.encounterCount > 0 }.lastSource)
    }

    @Test
    fun discoveryRejectsMalformedDuplicateOutOfRangeSourceAndNonExactKeys() {
        val valid = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            EncounterDiscoveryRecord(creature.packId, 0, null)
        }
        val invalidPages = listOf(
            EncounterDiscoveryPage("wrong", valid),
            EncounterDiscoveryPage("kitsu.encounter-discovery.v1", valid.dropLast(1)),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.dropLast(1) + valid.first(),
            ),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.mapIndexed { index, item ->
                    if (index == 0) item.copy(packId = 4_294_967_296L) else item
                },
            ),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.mapIndexed { index, item ->
                    if (index == 0) item.copy(encounterCount = 65_536, lastSource = "mesh_peer") else item
                },
            ),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.mapIndexed { index, item ->
                    if (index == 0) item.copy(encounterCount = 0, lastSource = "mesh_peer") else item
                },
            ),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.mapIndexed { index, item ->
                    if (index == 0) item.copy(encounterCount = 1, lastSource = null) else item
                },
            ),
            EncounterDiscoveryPage(
                "kitsu.encounter-discovery.v1",
                valid.mapIndexed { index, item ->
                    if (index == 0) item.copy(encounterCount = 1, lastSource = "direct_encounter") else item
                },
            ),
        )
        invalidPages.forEach { page ->
            val failure = runCatching {
                EncounterWireCodec.discovery(exactJson.encodeToString(page).toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_encounter_discovery", failure.code)
        }

        val validJson = exactJson.encodeToString(
            EncounterDiscoveryPage("kitsu.encounter-discovery.v1", valid),
        )
        listOf(
            validJson.replaceFirst("{\"schema\"", "{\"extra\":1,\"schema\""),
            validJson.replaceFirst("\"last_source\":null", "\"last_source\":null,\"extra\":1"),
        ).forEach { payload ->
            val failure = runCatching {
                EncounterWireCodec.discovery(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_encounter_discovery", failure.code)
        }
    }

    @Test
    fun codePageRejectsUnknownRarityAndDuplicateOpaqueIds() {
        val unknown = runCatching {
            EncounterWireCodec.codePage(
                """{"schema":"kitsu.encounter-codes.v1","items":[{"device_id":"KT12AF","code_id":"a","code":"K8-ABCDE-FGHJK-MNPQR","pack_id":1,"rarity":"ultra","acquired_at_epoch":1}]}"""
                    .toByteArray(),
            )
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_encounter_codes", unknown.code)

        val duplicate = runCatching {
            EncounterWireCodec.codePage(
                """{"schema":"kitsu.encounter-codes.v1","items":[
                    {"device_id":"KT12AF","code_id":"a","code":"K8-ABCDE-FGHJK-MNPQR","pack_id":1,"rarity":"rare","acquired_at_epoch":1},
                    {"device_id":"KT12AF","code_id":"a","code":"K8-STVWZ-23456-789AB","pack_id":2,"rarity":"epic","acquired_at_epoch":2}
                ]}""".trimIndent().toByteArray(),
            )
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_encounter_codes", duplicate.code)
    }

    @Test
    fun neighborPetContractIsSeparateAndTargeted() {
        val command = NeighborInteractionCommand(
            actionId = "00000000-0000-0000-0000-000000000007",
            targetDeviceId = "KT12AF",
            targetSessionNonce = 4_294_967_295L,
            sequence = 65_535,
            kind = NeighborInteractionKind.PET,
            expiresAtEpoch = 1_787_600_030,
        )
        val body = EncounterWireCodec.neighborActionBody(command)
        assertEquals("KT12AF", body.getValue("target_device_id").jsonPrimitive.content)
        assertEquals("pet", body.getValue("kind").jsonPrimitive.content)
        assertFalse(body.containsKey("local_care"))

        mapOf(
            NeighborInteractionKind.GREET to "greet",
            NeighborInteractionKind.PLAY to "play",
            NeighborInteractionKind.GIFT to "gift",
        ).forEach { (kind, wireName) ->
            val kindBody = EncounterWireCodec.neighborActionBody(command.copy(kind = kind))
            assertEquals(wireName, kindBody.getValue("kind").jsonPrimitive.content)
            assertEquals(command.targetDeviceId, kindBody.getValue("target_device_id").jsonPrimitive.content)
        }

        val receipt = EncounterWireCodec.neighborActionReceipt(
            """{"schema":"kitsu.neighbor-action-receipt.v1","action_id":"${command.actionId}","accepted":true,"state":"delivered","future":1}"""
                .toByteArray(),
            command,
        )
        assertTrue(receipt.accepted)

        val outOfRange = runCatching {
            EncounterWireCodec.neighborActionBody(command.copy(sequence = 65_536))
        }.exceptionOrNull() as TransportException
        assertEquals("invalid_neighbor_sequence", outOfRange.code)
    }

    @Test
    fun nearbyKitsuPageAcceptsExactFirmwareContractAndOrdersFreshestFirst() {
        val page = EncounterWireCodec.nearbyKitsu(
            """{
                "schema":"kitsu.encounter-neighbors.v1",
                "items":[
                    {"device_id":"KT12AF","session_nonce":4294967295,"pack_id":1554810531,"appearance":31,"evolution_stage":4,"bond":100,"mood":14,"emote":15,"rssi":-88.5,"snr":4.0,"last_seen_age_ms":9000,"next_sequence":65535},
                    {"device_id":"KT0001","session_nonce":7,"pack_id":1815690785,"appearance":0,"evolution_stage":0,"bond":0,"mood":0,"emote":0,"rssi":-61.0,"snr":9.5,"last_seen_age_ms":20,"next_sequence":1,"future":true}
                ],
                "future_page_field":true
            }""".trimIndent().toByteArray(),
        )

        assertEquals(listOf("KT0001", "KT12AF"), page.items.map { it.deviceId })
        assertEquals(65_535L, page.items.last().nextSequence)
        assertEquals(listOf(NeighborInteractionKind.PET), page.supportedActions)
    }

    @Test
    fun nearbyKitsuPageCapabilityEnablesOnlyExplicitAuthenticatedActions() {
        val page = EncounterWireCodec.nearbyKitsu(
            """{
                "schema":"kitsu.encounter-neighbors.v1",
                "supported_actions":["pet","greet","play","gift"],
                "items":[]
            }""".trimIndent().toByteArray(),
        )

        assertEquals(
            listOf(
                NeighborInteractionKind.PET,
                NeighborInteractionKind.GREET,
                NeighborInteractionKind.PLAY,
                NeighborInteractionKind.GIFT,
            ),
            page.supportedActions,
        )

        listOf(
            """{"schema":"kitsu.encounter-neighbors.v1","supported_actions":[],"items":[]}""",
            """{"schema":"kitsu.encounter-neighbors.v1","supported_actions":["greet"],"items":[]}""",
            """{"schema":"kitsu.encounter-neighbors.v1","supported_actions":["pet","pet"],"items":[]}""",
            """{"schema":"kitsu.encounter-neighbors.v1","supported_actions":["pet","dance"],"items":[]}""",
        ).forEach { payload ->
            val failure = runCatching {
                EncounterWireCodec.nearbyKitsu(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_encounter_neighbors", failure.code)
        }
    }

    @Test
    fun nearbyKitsuPageRejectsDuplicateStaleAndOutOfRangeRecords() {
        val invalidPayloads = listOf(
            """{"schema":"kitsu.encounter-neighbors.v1","items":[
                {"device_id":"KT0001","session_nonce":1,"pack_id":1,"appearance":0,"evolution_stage":0,"bond":0,"mood":0,"emote":0,"rssi":-70.0,"snr":1.0,"last_seen_age_ms":1,"next_sequence":1},
                {"device_id":"KT0001","session_nonce":2,"pack_id":2,"appearance":0,"evolution_stage":0,"bond":0,"mood":0,"emote":0,"rssi":-70.0,"snr":1.0,"last_seen_age_ms":1,"next_sequence":1}
            ]}""",
            """{"schema":"kitsu.encounter-neighbors.v1","items":[
                {"device_id":"KT0001","session_nonce":1,"pack_id":1,"appearance":0,"evolution_stage":0,"bond":0,"mood":0,"emote":0,"rssi":-70.0,"snr":1.0,"last_seen_age_ms":120001,"next_sequence":1}
            ]}""",
            """{"schema":"kitsu.encounter-neighbors.v1","items":[
                {"device_id":"KT0001","session_nonce":1,"pack_id":1,"appearance":32,"evolution_stage":0,"bond":0,"mood":0,"emote":0,"rssi":-70.0,"snr":1.0,"last_seen_age_ms":1,"next_sequence":0}
            ]}""",
        )

        invalidPayloads.forEach { payload ->
            val failure = runCatching {
                EncounterWireCodec.nearbyKitsu(payload.toByteArray())
            }.exceptionOrNull() as TransportException
            assertEquals("malformed_encounter_neighbors", failure.code)
        }
    }
}
