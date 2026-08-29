package ptl.kitsu.app

import ptl.kitsu.app.cache.CachePolicy
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.EncounterDiscoveryCachePolicy
import ptl.kitsu.app.cache.OwnerCacheBindingPolicy
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionPolicy
import ptl.kitsu.app.model.ChannelRegionScope
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshState
import ptl.kitsu.app.model.RepeatSource
import ptl.kitsu.app.security.SafeLog
import ptl.kitsu.app.ui.locationSettingsActionState
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PolicyAndStorageTest {
    private val validPublicKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun messageLimitCountsUtf8Bytes() {
        val command = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000001",
            targetId = validPublicKey,
            text = "🦊".repeat(33),
            messageRoute = MessageRoute.DIRECT,
        )
        assertEquals("text_too_long", ActionPolicy.validate(command))
    }

    @Test fun messageRecipientContractRejectsAliasesAndNonCanonicalChannels() {
        val base = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000001",
            targetId = "peer-1",
            text = "hello",
            messageRoute = MessageRoute.DIRECT,
        )
        assertEquals("invalid_peer_public_key", ActionPolicy.validate(base))
        assertEquals(
            "invalid_channel_slot",
            ActionPolicy.validate(base.copy(targetId = "04", messageRoute = MessageRoute.CHANNEL)),
        )
        assertNull(ActionPolicy.validate(base.copy(targetId = "0", messageRoute = MessageRoute.CHANNEL)))
        assertNull(ActionPolicy.validate(base.copy(targetId = validPublicKey)))
    }

    @Test fun logsRedactEveryCredentialClass() {
        val rendered = SafeLog.render(
            "event",
            mapOf(
                "access_token" to "raw",
                "privateKey" to "hunter2",
                "pairingDevice" to "private-device",
                "secret" to "correct horse battery staple",
                "encounter_code" to "K8-ABCDE-FGHJK-MNPQR",
                "count" to 2,
            ),
        )
        assertFalse(rendered.contains("raw"))
        assertFalse(rendered.contains("hunter2"))
        assertFalse(rendered.contains("private-device"))
        assertFalse(rendered.contains("correct horse battery staple"))
        assertFalse(rendered.contains("K8-ABCDE-FGHJK-MNPQR"))
        assertTrue(rendered.contains("count=2"))
    }

    @Test fun cacheIsBoundedWithoutDatabase() {
        val entries = (0..300).map {
            HistoryEntry("$it", "c:$it", "test", "entry", it.toLong())
        }
        val bounded = CachePolicy.bounded(CacheSnapshot(history = entries, writtenAt = 1))
        assertEquals(CachePolicy.MAX_HISTORY, bounded.history.size)
        assertEquals("45", bounded.history.first().id)
    }

    @Test fun discoveryCacheRestoresOnlyForTheExactSavedDeviceAndAuthenticatedDeviceId() {
        val records = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            EncounterDiscoveryRecord(creature.packId, 0, null)
        }
        val snapshot = CacheSnapshot(
            status = KitsuStatus(deviceId = "KT0001", companionName = "Fox", updatedAt = 1),
            writtenAt = 1,
            deviceAddress = "00:11:22:33:44:55",
            encounterDiscoveryDeviceId = "KT0001",
            encounterDiscovery = records,
        )

        assertEquals(
            records,
            EncounterDiscoveryCachePolicy.restore(snapshot, "00:11:22:33:44:55")?.records,
        )
        assertNull(EncounterDiscoveryCachePolicy.restore(snapshot, "AA:BB:CC:DD:EE:FF"))
        assertNull(
            EncounterDiscoveryCachePolicy.restore(
                snapshot.copy(encounterDiscoveryDeviceId = "KT0002"),
                "00:11:22:33:44:55",
            ),
        )
        assertNull(
            EncounterDiscoveryCachePolicy.restore(
                snapshot.copy(encounterDiscovery = records.dropLast(1)),
                "00:11:22:33:44:55",
            ),
        )
    }

    @Test fun entireCachedOwnerSnapshotRequiresTheExactSavedDeviceAddress() {
        val snapshot = CacheSnapshot(
            status = KitsuStatus(deviceId = "KT-A", companionName = "A", updatedAt = 1),
            history = listOf(HistoryEntry("A:1", "1", "mesh", "heard", 1)),
            messages = listOf(
                Message(
                    id = "A:1",
                    cursor = "1",
                    direction = "inbound",
                    text = "hello",
                    state = "received",
                    occurredAt = 1,
                ),
            ),
            writtenAt = 1,
            deviceAddress = "AA:BB:CC:DD:EE:FF",
        )

        val restored = OwnerCacheBindingPolicy.restore(snapshot, "aa:bb:cc:dd:ee:ff")
        assertEquals("KT-A", restored?.status?.deviceId)
        assertEquals(listOf("A:1"), restored?.history?.map(HistoryEntry::id))
        assertEquals(listOf("A:1"), restored?.messages?.map(Message::id))
        assertNull(OwnerCacheBindingPolicy.restore(snapshot, "00:11:22:33:44:55"))
        assertNull(OwnerCacheBindingPolicy.restore(snapshot, null))
        assertNull(
            OwnerCacheBindingPolicy.restore(
                snapshot.copy(deviceAddress = null),
                "AA:BB:CC:DD:EE:FF",
            ),
        )
    }

    @Test fun cacheKeepsRepeatCountAndReadsRowsWrittenBeforeTheFieldExisted() {
        val json = Json { ignoreUnknownKeys = true }
        val legacy = json.decodeFromString(
            CacheSnapshot.serializer(),
            """{"messages":[{"id":"1","cursor":"1","direction":"outbound","channel":"0","text":"hello","state":"sent","occurredAt":1}],"writtenAt":1}""",
        )
        assertNull(legacy.messages.single().repeatCount)
        assertNull(legacy.messages.single().repeatObservationOpen)
        assertNull(legacy.messages.single().repeatSources)
        assertNull(legacy.messages.single().repeatSourcesTruncated)

        val snapshot = CacheSnapshot(
            messages = listOf(
                Message(
                    id = "2",
                    cursor = "2",
                    direction = "outbound",
                    channel = "0",
                    text = "hello",
                    state = "sent",
                    occurredAt = 2,
                    repeatCount = 12,
                ),
            ),
            writtenAt = 2,
        )
        val restored = json.decodeFromString(
            CacheSnapshot.serializer(),
            json.encodeToString(CacheSnapshot.serializer(), snapshot),
        )

        assertEquals(12, restored.messages.single().repeatCount)
    }

    @Test fun cacheKeepsIndependentAdvertRecordsAndReadsStatusWrittenBeforeTheFieldsExisted() {
        val json = Json { ignoreUnknownKeys = true }
        val legacy = json.decodeFromString(
            CacheSnapshot.serializer(),
            """{"status":{"deviceId":"KTOLD","companionName":"Fox","updatedAt":1},"writtenAt":1}""",
        )
        assertNull(legacy.status?.mesh?.lastFloodAdvert)
        assertNull(legacy.status?.mesh?.lastNearbyAdvert)

        val evidence = LastFloodAdvert(
            emittedAt = 1_787_000_000,
            state = "sent",
            repeatCount = 4,
            observationOpen = true,
            repeatSources = listOf(RepeatSource("00")),
            repeatSourcesTruncated = false,
        )
        val snapshot = CacheSnapshot(
            status = KitsuStatus(
                deviceId = "KTNEW",
                companionName = "Fox",
                mesh = MeshState(
                    lastFloodAdvert = evidence,
                    lastNearbyAdvert = LastNearbyAdvert(1_787_000_100, "sent"),
                ),
                updatedAt = 2,
            ),
            writtenAt = 2,
        )
        val restored = json.decodeFromString(
            CacheSnapshot.serializer(),
            json.encodeToString(CacheSnapshot.serializer(), snapshot),
        )

        assertEquals(evidence, restored.status?.mesh?.lastFloodAdvert)
        assertEquals("00", restored.status?.mesh?.lastFloodAdvert?.repeatSources?.single()?.lastHopToken)
        assertEquals("sent", restored.status?.mesh?.lastNearbyAdvert?.state)
    }

    @Test fun cachePreservesChannelRoutingAndOldSnapshotsRemainCompatible() {
        val json = Json { ignoreUnknownKeys = true }
        val legacy = json.decodeFromString(
            CacheSnapshot.serializer(),
            """{"writtenAt":1}""",
        )
        assertTrue(legacy.channels.isEmpty())

        val snapshot = CacheSnapshot(
            channels = listOf(
                MeshChannel(0, true, "Public"),
                MeshChannel(1, true, "Ops", ChannelRegionScope.EU),
            ),
            writtenAt = 2,
        )
        val restored = json.decodeFromString(
            CacheSnapshot.serializer(),
            json.encodeToString(CacheSnapshot.serializer(), snapshot),
        )

        assertEquals(null, restored.channels[0].regionScope)
        assertEquals(ChannelRegionScope.EU, restored.channels[1].regionScope)
    }

    @Test fun legacyBleLocationFailuresExposeOtaLockedRecovery() {
        val disabled = locationSettingsActionState("location_services_disabled", null, updateBusy = false)
        assertTrue(disabled.visible)
        assertTrue(disabled.enabled)

        val unavailable = locationSettingsActionState("disconnected", "location_services_unavailable", updateBusy = false)
        assertTrue(unavailable.visible)
        assertTrue(unavailable.enabled)

        val updating = locationSettingsActionState("location_services_disabled", null, updateBusy = true)
        assertTrue(updating.visible)
        assertFalse(updating.enabled)

        assertFalse(locationSettingsActionState("bluetooth_disabled", null, updateBusy = false).visible)
    }
}
