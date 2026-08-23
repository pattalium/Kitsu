package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.ChannelRegionScope
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.model.MeshState
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageMarkReadReceipt
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.model.RepeatSource
import ptl.kitsu.app.model.WIRE_VERSION
import ptl.kitsu.app.update.FirmwareUpdatePackageReader
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** Maps compact authenticated Heltec payloads into local app models. */
internal object FirmwareBlePayloadMapper {
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }
    private val strictJson = Json { ignoreUnknownKeys = false; explicitNulls = false }

    @Serializable
    private data class FirmwareState(
        val schema: String,
        @SerialName("device_uid") val deviceUid: String,
        val companion: String,
        @SerialName("firmware_version") val firmwareVersion: String? = null,
        val listening: Boolean = false,
        val mood: String? = null,
        @SerialName("battery_percent") val batteryPercent: Int? = null,
        @SerialName("battery_mv") val batteryMillivolts: Int? = null,
        @SerialName("pack_ready") val packReady: Boolean = false,
        @SerialName("pack_id") val packId: Long? = null,
        @SerialName("pack_revision") val packRevision: Long? = null,
        @SerialName("bond_level") val bondLevel: Int = 0,
        @SerialName("bond_xp") val bondExperience: Int = 0,
        @SerialName("bond_progress_percent") val bondProgressPercent: Int = 0,
        @SerialName("evolution_stage") val evolutionStage: String? = null,
        @SerialName("appearance_variant") val appearanceVariant: Int? = null,
        val personality: String? = null,
        @SerialName("unlock_mask") val unlockMask: Long = 0,
        @SerialName("memory_count") val memoryCount: Int = 0,
        val energy: Int,
        val curiosity: Int,
        val affection: Int,
        val sleeping: Boolean,
        @SerialName("mesh_rx_ready") val meshRxReady: Boolean,
        @SerialName("mesh_enabled") val meshEnabled: Boolean = false,
        @SerialName("mesh_time_valid") val meshTimeValid: Boolean = false,
        @SerialName("mesh_one_shot_ready") val meshOneShotReady: Boolean = false,
        @SerialName("mesh_identity_ready") val meshIdentityReady: Boolean? = null,
        @SerialName("mesh_advertise_ready") val meshAdvertiseReady: Boolean? = null,
        @SerialName("mesh_advertise_retry_after_ms") val meshAdvertiseRetryAfterMs: Long? = null,
        @SerialName("mesh_advertise_error") val meshAdvertiseError: String? = null,
        @SerialName("mesh_last_flood_advert") val meshLastFloodAdvert: JsonObject? = null,
        @SerialName("mesh_last_flood_advert_v2") val meshLastFloodAdvertV2: JsonObject? = null,
        @SerialName("mesh_last_nearby_advert") val meshLastNearbyAdvert: JsonObject? = null,
        @SerialName("event_count") val eventCount: Int = 0,
    )

    @Serializable
    private data class FirmwareLastFloodAdvert(
        @SerialName("emitted_at") val emittedAt: Long,
        val state: String,
        @SerialName("repeat_count") val repeatCount: Int?,
        @SerialName("observation_open") val observationOpen: Boolean,
    )

    @Serializable
    private data class FirmwareLastNearbyAdvert(
        @SerialName("emitted_at") val emittedAt: Long,
        val state: String,
        @SerialName("repeat_count") val repeatCount: Int?,
        @SerialName("observation_open") val observationOpen: Boolean,
    )

    @Serializable
    private data class FirmwareLastFloodAdvertV2(
        @SerialName("emitted_at") val emittedAt: Long,
        val state: String,
        @SerialName("repeat_count") val repeatCount: Int?,
        @SerialName("observation_open") val observationOpen: Boolean,
        @SerialName("repeat_sources") val repeatSources: List<FirmwareRepeatSource>?,
        @SerialName("repeat_sources_truncated") val repeatSourcesTruncated: Boolean?,
    )

    @Serializable private data class Observation(
        @SerialName("epoch_valid") val epochValid: Boolean,
        val epoch: Long,
    )

    @Serializable private data class LastHop(val valid: Boolean, val rssi: Double, val snr: Double)

    @Serializable private data class FirmwareHistoryItem(
        val sequence: String,
        @SerialName("public_key_b64") val publicKeyB64: String,
        val observed: Observation,
        @SerialName("last_hop") val lastHop: LastHop,
    )

    @Serializable private data class FirmwareHistoryPage(
        val schema: String,
        val items: List<FirmwareHistoryItem>,
        val cursor: String? = null,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwarePeer(
        @SerialName("public_key_b64") val publicKeyB64: String,
        val name: String,
        val type: Int,
        @SerialName("last_observed") val lastObserved: Observation,
        @SerialName("last_hop") val lastHop: LastHop,
    )

    @Serializable private data class FirmwarePeerPage(val schema: String, val items: List<FirmwarePeer>)

    @Serializable private data class FirmwareMessageV1(
        @SerialName("message_id") val messageId: String,
        val timestamp: Long,
        val inbound: Boolean,
        val kind: String,
        val authenticated: Boolean,
        @SerialName("sender_name") val senderName: String,
        @SerialName("peer_id") val peerId: String? = null,
        @SerialName("channel_slot") val channelSlot: Int? = null,
        val text: String,
        val state: String,
    )

    @Serializable private data class FirmwareMessagePageV1(
        val schema: String,
        val items: List<FirmwareMessageV1>,
        val cursor: String? = null,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwareMessageV2(
        @SerialName("message_id") val messageId: String,
        val revision: String,
        val timestamp: Long,
        val inbound: Boolean,
        val kind: String,
        @SerialName("peer_id") val peerId: String?,
        @SerialName("channel_slot") val channelSlot: Int?,
        val authenticated: Boolean,
        val unread: Boolean,
        @SerialName("sender_name") val senderName: String,
        val text: String,
        val state: String,
        val route: String,
        @SerialName("local_tx") val localTx: String,
        @SerialName("delivery_ack") val deliveryAck: String,
        @SerialName("repeater_count") val repeaterCount: Int?,
        @SerialName("repeaters_heard") val repeatersHeard: Int?,
        @SerialName("rssi_dbm") val rssiDbm: Double?,
        @SerialName("snr_db") val snrDb: Double?,
    )

    @Serializable private data class FirmwareMessagePageV2(
        val schema: String,
        @SerialName("journal_session") val journalSession: String,
        @SerialName("journal_revision") val journalRevision: String,
        val items: List<FirmwareMessageV2>,
        val cursor: String?,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwareMessageV3(
        @SerialName("message_id") val messageId: String,
        val revision: String,
        val timestamp: Long,
        val inbound: Boolean,
        val kind: String,
        @SerialName("peer_id") val peerId: String?,
        @SerialName("channel_slot") val channelSlot: Int?,
        val authenticated: Boolean,
        val unread: Boolean,
        @SerialName("sender_name") val senderName: String,
        val text: String,
        val state: String,
        val route: String,
        @SerialName("local_tx") val localTx: String,
        @SerialName("delivery_ack") val deliveryAck: String,
        @SerialName("repeater_count") val repeaterCount: Int?,
        @SerialName("repeaters_heard") val repeatersHeard: Int?,
        @SerialName("repeat_count") val repeatCount: Int?,
        @SerialName("rssi_dbm") val rssiDbm: Double?,
        @SerialName("snr_db") val snrDb: Double?,
    )

    @Serializable private data class FirmwareMessagePageV3(
        val schema: String,
        @SerialName("journal_session") val journalSession: String,
        @SerialName("journal_revision") val journalRevision: String,
        val items: List<FirmwareMessageV3>,
        val cursor: String?,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwareMessageV4(
        @SerialName("message_id") val messageId: String,
        val revision: String,
        val timestamp: Long,
        val inbound: Boolean,
        val kind: String,
        @SerialName("peer_id") val peerId: String?,
        @SerialName("channel_slot") val channelSlot: Int?,
        val authenticated: Boolean,
        val unread: Boolean,
        @SerialName("sender_name") val senderName: String,
        val text: String,
        val state: String,
        val route: String,
        @SerialName("local_tx") val localTx: String,
        @SerialName("delivery_ack") val deliveryAck: String,
        @SerialName("repeater_count") val repeaterCount: Int?,
        @SerialName("repeaters_heard") val repeatersHeard: Int?,
        @SerialName("repeat_count") val repeatCount: Int?,
        @SerialName("repeat_observation_open") val repeatObservationOpen: Boolean?,
        @SerialName("repeat_sources") val repeatSources: List<FirmwareRepeatSource>?,
        @SerialName("repeat_sources_truncated") val repeatSourcesTruncated: Boolean?,
        @SerialName("rssi_dbm") val rssiDbm: Double?,
        @SerialName("snr_db") val snrDb: Double?,
    )

    @Serializable private data class FirmwareRepeatSource(
        @SerialName("last_hop_token") val lastHopToken: String,
    )

    @Serializable private data class FirmwareMessagePageV4(
        val schema: String,
        @SerialName("journal_session") val journalSession: String,
        @SerialName("journal_revision") val journalRevision: String,
        val items: List<FirmwareMessageV4>,
        val cursor: String?,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwareChannelV1(
        val slot: Int,
        val configured: Boolean,
        val name: String?,
    )
    @Serializable private data class FirmwareChannelPageV1(
        val schema: String,
        val items: List<FirmwareChannelV1>,
    )
    @Serializable private data class FirmwareChannelV2(
        val slot: Int,
        val configured: Boolean,
        val name: String?,
        @SerialName("region_scope") val regionScope: String?,
    )
    @Serializable private data class FirmwareChannelPageV2(
        val schema: String,
        val items: List<FirmwareChannelV2>,
    )
    @Serializable private data class FirmwareMeshConfiguration(
        val schema: String,
        val enabled: Boolean,
        val profile: String,
        @SerialName("tx_power_dbm") val txPowerDbm: Int,
    )

    fun rejectionCode(payload: ByteArray): String? = try {
        val root = json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
        if (root["ok"]?.jsonPrimitive?.booleanOrNull == false) {
            root["error"]?.jsonPrimitive?.content ?: "request_rejected"
        } else null
    } catch (_: Throwable) {
        null
    }

    fun event(operation: String, payload: ByteArray): EventEnvelope {
        val wire = payload.toString(Charsets.UTF_8)
        val value = try {
            strictJson.decodeFromString<EventEnvelope>(wire)
        } catch (failure: Throwable) {
            throw TransportException("malformed_event", failure)
        }
        val refreshSequence = value.cursor.removePrefix("ble:").toLongOrNull()
        if (operation != "companion.refresh" || value.v != WIRE_VERSION || value.kind != "refresh" ||
            value.body.isNotEmpty() || refreshSequence == null || value.cursor != "ble:$refreshSequence" ||
            refreshSequence !in 1L..0xffffffffL
        ) throw TransportException("malformed_event")
        return value
    }

    fun state(payload: ByteArray, observedAtEpochSeconds: Long): KitsuStatus {
        val rawState = try {
            json.parseToJsonElement(payload.toString(Charsets.UTF_8)).jsonObject
        } catch (failure: Throwable) {
            throw TransportException("malformed_state", failure)
        }
        val value = decode<FirmwareState>(payload, "malformed_state")
        requireSchema(value.schema, "kitsu.state.v1")
        val enhancedFirmware = FirmwareMessageApiPolicy.protocolVersion(value.firmwareVersion) >= 4
        val lastFloodAdvert = if (enhancedFirmware) {
            if (!rawState.containsKey("mesh_last_flood_advert") ||
                !rawState.containsKey("mesh_last_flood_advert_v2") ||
                !rawState.containsKey("mesh_last_nearby_advert")
            ) {
                throw TransportException("malformed_state")
            }
            val legacyRaw = rawState["mesh_last_flood_advert"]
            val enhancedRaw = rawState["mesh_last_flood_advert_v2"]
            when {
                legacyRaw === JsonNull && enhancedRaw === JsonNull -> null
                legacyRaw is JsonObject && enhancedRaw is JsonObject -> {
                    val legacy = lastFloodAdvert(legacyRaw)
                    val enhanced = lastFloodAdvertV2(enhancedRaw)
                    if (legacy.emittedAt != enhanced.emittedAt || legacy.state != enhanced.state ||
                        legacy.repeatCount != enhanced.repeatCount ||
                        legacy.observationOpen != enhanced.observationOpen
                    ) {
                        throw TransportException("malformed_state")
                    }
                    enhanced
                }
                else -> throw TransportException("malformed_state")
            }
        } else {
            value.meshLastFloodAdvert?.let(::lastFloodAdvert)
        }
        val lastNearbyAdvert = value.meshLastNearbyAdvert?.let(::lastNearbyAdvert)
        val advertiseFieldsPresent = listOf(
            value.meshIdentityReady,
            value.meshAdvertiseReady,
            value.meshAdvertiseRetryAfterMs,
        ).all { it != null }
        val advertiseFieldsAbsent = listOf(
            value.meshIdentityReady,
            value.meshAdvertiseReady,
            value.meshAdvertiseRetryAfterMs,
        ).all { it == null }
        val advertiseReadinessConsistent = !advertiseFieldsPresent ||
            ((value.meshAdvertiseError == null) == (value.meshAdvertiseReady == true))
        if (value.energy !in 0..100 || value.curiosity !in 0..100 || value.affection !in 0..100 ||
            (value.batteryPercent != null && value.batteryPercent !in 0..100) ||
            (value.batteryMillivolts != null && value.batteryMillivolts < 0) ||
            (value.packId != null && value.packId !in 0..0xffffffffL) ||
            (value.packRevision != null && value.packRevision !in 0..0xffffffffL) ||
            value.bondLevel < 0 || value.bondExperience < 0 || value.bondProgressPercent !in 0..100 ||
            (value.appearanceVariant != null && value.appearanceVariant !in 0..255) ||
            value.unlockMask < 0 || value.memoryCount < 0 ||
            (!advertiseFieldsPresent && !advertiseFieldsAbsent) ||
            (value.meshAdvertiseRetryAfterMs != null && value.meshAdvertiseRetryAfterMs !in 0L..30_000L) ||
            (value.meshAdvertiseError != null && value.meshAdvertiseError !in ADVERTISE_ERRORS) ||
            !advertiseReadinessConsistent ||
            (value.meshAdvertiseReady == true &&
                (value.meshIdentityReady != true || !value.meshEnabled || !value.meshTimeValid ||
                    value.meshAdvertiseRetryAfterMs != 0L))
        ) {
            throw TransportException("malformed_state")
        }
        return KitsuStatus(
            deviceId = value.deviceUid,
            companionName = value.companion,
            firmwareVersion = value.firmwareVersion,
            listening = value.listening,
            mood = value.mood?.takeIf { it.isNotBlank() } ?: if (value.sleeping) "SLEEPING" else "AWAKE",
            batteryPercent = value.batteryPercent,
            batteryMillivolts = value.batteryMillivolts,
            packReady = value.packReady,
            packId = value.packId?.toString(),
            packRevision = value.packRevision,
            bondLevel = value.bondLevel,
            bondExperience = value.bondExperience,
            bondProgressPercent = value.bondProgressPercent,
            evolutionStage = value.evolutionStage,
            appearanceVariant = value.appearanceVariant?.toString(),
            personality = value.personality,
            unlockMask = value.unlockMask,
            memoryCount = value.memoryCount,
            needs = NeedLevels(value.energy, value.curiosity, value.affection),
            mesh = MeshState(
                enabled = value.meshEnabled,
                rxReady = value.meshRxReady,
                timeValid = value.meshTimeValid,
                oneShotReady = value.meshOneShotReady,
                advertiseSupported = advertiseFieldsPresent,
                identityReady = value.meshIdentityReady == true,
                advertiseReady = value.meshAdvertiseReady == true,
                advertiseRetryAfterMs = value.meshAdvertiseRetryAfterMs ?: 0L,
                advertiseError = value.meshAdvertiseError,
                lastFloodAdvert = lastFloodAdvert,
                lastNearbyAdvert = lastNearbyAdvert,
            ),
            cursor = value.eventCount.takeIf { it > 0 }?.toString(),
            updatedAt = observedAtEpochSeconds,
        )
    }

    /**
     * state.v1 is deliberately forward-compatible, but this optional evidence object
     * is an atomic producer contract: all four known keys must be present and valid.
     */
    private fun lastFloodAdvert(raw: JsonObject): LastFloodAdvert {
        if (raw.keys != LAST_FLOOD_ADVERT_KEYS) throw TransportException("malformed_state")
        val value = try {
            strictJson.decodeFromJsonElement<FirmwareLastFloodAdvert>(raw)
        } catch (failure: Throwable) {
            throw TransportException("malformed_state", failure)
        }
        val validState = when (value.state) {
            "queued" -> value.repeatCount == null && !value.observationOpen
            "sent" -> value.repeatCount?.let { it in 0..255 } == true
            "tx_failed" -> value.repeatCount == null && !value.observationOpen
            else -> false
        }
        if (value.emittedAt !in MIN_VALID_MESH_EPOCH..MAX_VALID_MESH_EPOCH || !validState) {
            throw TransportException("malformed_state")
        }
        return LastFloodAdvert(
            emittedAt = value.emittedAt,
            state = value.state,
            repeatCount = value.repeatCount,
            observationOpen = value.observationOpen,
        )
    }

    /**
     * state.v1 stays forward-compatible, while the optional Nearby record is an
     * exact zero-hop lifecycle cluster. It can never carry repeat observations.
     */
    private fun lastNearbyAdvert(raw: JsonObject): LastNearbyAdvert {
        if (raw.keys != LAST_NEARBY_ADVERT_KEYS) throw TransportException("malformed_state")
        val value = try {
            strictJson.decodeFromJsonElement<FirmwareLastNearbyAdvert>(raw)
        } catch (failure: Throwable) {
            throw TransportException("malformed_state", failure)
        }
        if (value.emittedAt !in MIN_VALID_MESH_EPOCH..MAX_VALID_MESH_EPOCH ||
            value.state !in ADVERT_LIFECYCLE_STATES || value.repeatCount != null ||
            value.observationOpen
        ) {
            throw TransportException("malformed_state")
        }
        return LastNearbyAdvert(emittedAt = value.emittedAt, state = value.state)
    }

    /** Strict additive 0.16.1+ cluster; the legacy four-key state remains unchanged. */
    private fun lastFloodAdvertV2(raw: JsonObject): LastFloodAdvert {
        if (raw.keys != LAST_FLOOD_ADVERT_V2_KEYS) throw TransportException("malformed_state")
        val value = try {
            strictJson.decodeFromJsonElement<FirmwareLastFloodAdvertV2>(raw)
        } catch (failure: Throwable) {
            throw TransportException("malformed_state", failure)
        }
        val validState = when (value.state) {
            "queued", "tx_failed" -> value.repeatCount == null && !value.observationOpen &&
                value.repeatSources == null && value.repeatSourcesTruncated == null
            "sent" -> value.repeatCount?.let { it in 0..255 } == true &&
                value.repeatSources != null && value.repeatSourcesTruncated != null &&
                value.repeatSources.size <= 4 &&
                value.repeatSources.size <= value.repeatCount &&
                value.repeatSources.map(FirmwareRepeatSource::lastHopToken).distinct().size ==
                    value.repeatSources.size &&
                value.repeatSources.all(::validRepeatSource) &&
                (!value.repeatSourcesTruncated ||
                    (value.repeatSources.size == 4 && value.repeatCount >= 5))
            else -> false
        }
        if (value.emittedAt !in MIN_VALID_MESH_EPOCH..MAX_VALID_MESH_EPOCH || !validState) {
            throw TransportException("malformed_state")
        }
        return LastFloodAdvert(
            emittedAt = value.emittedAt,
            state = value.state,
            repeatCount = value.repeatCount,
            observationOpen = value.observationOpen,
            repeatSources = value.repeatSources?.map { RepeatSource(it.lastHopToken) },
            repeatSourcesTruncated = value.repeatSourcesTruncated,
        )
    }

    fun history(payload: ByteArray): HistoryPage {
        val value = decode<FirmwareHistoryPage>(payload, "malformed_history")
        requireSchema(value.schema, "kitsu.history.v1")
        if (value.items.any { !MeshPeerKeyPolicy.isCanonicalBase64Url(it.publicKeyB64) }) {
            throw TransportException("malformed_history")
        }
        return HistoryPage(
            items = value.items.map { item ->
                val signal = if (item.lastHop.valid) " · ${item.lastHop.rssi} dBm last hop" else ""
                HistoryEntry(
                    id = item.sequence,
                    cursor = item.sequence,
                    kind = "mesh advert",
                    summary = "${item.publicKeyB64.take(10)}$signal",
                    occurredAt = item.observed.epoch.takeIf { item.observed.epochValid } ?: 0,
                )
            },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
        )
    }

    fun peers(payload: ByteArray): PeerPage {
        val value = decode<FirmwarePeerPage>(payload, "malformed_peers")
        requireSchema(value.schema, "kitsu.peers.v1")
        if (value.items.any { !MeshPeerKeyPolicy.isCanonicalBase64Url(it.publicKeyB64) }) {
            throw TransportException("malformed_peers")
        }
        return PeerPage(value.items.map { item ->
            Peer(
                id = item.publicKeyB64,
                name = item.name,
                role = when (item.type) { 1 -> "client"; 2 -> "repeater"; 3 -> "room"; 4 -> "sensor"; else -> "unknown" },
                lastHeardAt = item.lastObserved.epoch.takeIf { item.lastObserved.epochValid },
                route = if (item.lastHop.valid) "last hop ${item.lastHop.rssi} dBm / ${item.lastHop.snr} dB" else null,
            )
        })
    }

    fun messages(payload: ByteArray, expectedProtocolVersion: Int? = null): MessagePage {
        val wire = payload.toString(Charsets.UTF_8)
        val schema = try {
            strictJson.parseToJsonElement(wire).jsonObject["schema"]?.jsonPrimitive?.content
        } catch (failure: Throwable) {
            throw TransportException("malformed_messages", failure)
        }
        val page = when (schema) {
            "kitsu.messages.v1" -> messagesV1(wire)
            "kitsu.messages.v2" -> messagesV2(wire)
            "kitsu.messages.v3" -> messagesV3(wire)
            "kitsu.messages.v4" -> messagesV4(wire)
            else -> throw TransportException("unsupported_protocol")
        }
        if (expectedProtocolVersion != null && page.protocolVersion != expectedProtocolVersion) {
            throw TransportException("unsupported_protocol")
        }
        return page
    }

    fun messageMarkRead(payload: ByteArray): MessageMarkReadReceipt {
        val value = decodeStrict<MessageMarkReadReceipt>(
            payload.toString(Charsets.UTF_8),
            "malformed_message_read",
        )
        requireSchema(value.schema, "kitsu.messages-mark-read.v1")
        val countsValid = value.markedCount in 0..24 && value.unchangedCount in 0..24 &&
            value.markedCount + value.unchangedCount in 0..24
        val outcomeValid = if (value.accepted) {
            value.error == null && value.markedCount + value.unchangedCount in 1..24
        } else {
            value.error in MESSAGE_READ_ERRORS && value.markedCount == 0 && value.unchangedCount == 0
        }
        if (!validUint32(value.journalSession) ||
            !validUint32(value.journalRevision, allowZero = true) ||
            !countsValid || !outcomeValid
        ) throw TransportException("malformed_message_read")
        return value
    }

    private fun messagesV1(wire: String): MessagePage {
        val value = decodeStrict<FirmwareMessagePageV1>(wire, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v1")
        if (value.cursor?.let { !validUint32(it) } == true || value.items.any { !validMessageBase(
                messageId = it.messageId,
                timestamp = it.timestamp,
                inbound = it.inbound,
                kind = it.kind,
                authenticated = it.authenticated,
                peerId = it.peerId,
                channelSlot = it.channelSlot,
                senderName = it.senderName,
                text = it.text,
                state = it.state,
            )
        }) throw TransportException("malformed_messages")
        return MessagePage(
            items = value.items.map { item -> item.toMessage() },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
            protocolVersion = 1,
        )
    }

    private fun messagesV2(wire: String): MessagePage {
        val value = decodeStrict<FirmwareMessagePageV2>(wire, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v2")
        if (!validUint32(value.journalSession) ||
            !validUint32(value.journalRevision, allowZero = true) ||
            value.cursor?.let { !validUint32(it) } == true ||
            value.items.any { !validMessageV2(it) }
        ) throw TransportException("malformed_messages")
        return MessagePage(
            items = value.items.map { item -> item.toMessage(value.journalSession) },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
            journalSession = value.journalSession,
            journalRevision = value.journalRevision,
            protocolVersion = 2,
        )
    }

    private fun messagesV3(wire: String): MessagePage {
        if (!hasExactV3MessageItemKeys(wire)) throw TransportException("malformed_messages")
        val value = decodeStrict<FirmwareMessagePageV3>(wire, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v3")
        if (!validUint32(value.journalSession) ||
            !validUint32(value.journalRevision, allowZero = true) ||
            value.cursor?.let { !validUint32(it) } == true ||
            value.items.any { !validMessageV3(it) }
        ) throw TransportException("malformed_messages")
        return MessagePage(
            items = value.items.map { item -> item.toMessage(value.journalSession) },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
            journalSession = value.journalSession,
            journalRevision = value.journalRevision,
            protocolVersion = 3,
        )
    }

    private fun messagesV4(wire: String): MessagePage {
        if (!hasExactMessageItemKeys(wire, MESSAGE_V4_ITEM_KEYS)) {
            throw TransportException("malformed_messages")
        }
        val value = decodeStrict<FirmwareMessagePageV4>(wire, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v4")
        if (!validUint32(value.journalSession) ||
            !validUint32(value.journalRevision, allowZero = true) ||
            value.cursor?.let { !validUint32(it) } == true ||
            value.items.any { !validMessageV4(it) }
        ) throw TransportException("malformed_messages")
        return MessagePage(
            items = value.items.map { item -> item.toMessage(value.journalSession) },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
            journalSession = value.journalSession,
            journalRevision = value.journalRevision,
            protocolVersion = 4,
        )
    }

    private fun FirmwareMessageV1.toMessage() = Message(
        id = messageId,
        cursor = messageId,
        direction = if (inbound) "inbound" else "outbound",
        // Channel names are display-only and never become peer identities.
        peerId = if (kind == "direct") peerId else null,
        channel = channelSlot?.toString(),
        text = text,
        state = state,
        occurredAt = timestamp,
        revision = messageId,
        senderName = senderName,
    )

    private fun FirmwareMessageV2.toMessage(journalSession: String) = Message(
        id = messageId,
        cursor = messageId,
        direction = if (inbound) "inbound" else "outbound",
        peerId = if (kind == "direct") peerId else null,
        channel = channelSlot?.toString(),
        text = text,
        state = state,
        occurredAt = timestamp,
        revision = revision,
        journalSession = journalSession,
        senderName = senderName,
        unreadOnKitsu = unread,
        route = route,
        localTx = localTx,
        deliveryAck = deliveryAck,
        repeaterCount = repeaterCount,
        rssiDbm = rssiDbm,
        snrDb = snrDb,
    )

    private fun FirmwareMessageV3.toMessage(journalSession: String): Message =
        v2Shape().toMessage(journalSession).copy(repeatCount = repeatCount)

    private fun FirmwareMessageV4.toMessage(journalSession: String): Message =
        v3Shape().toMessage(journalSession).copy(
            repeatObservationOpen = repeatObservationOpen,
            repeatSources = repeatSources?.map { source ->
                ptl.kitsu.app.model.RepeatSource(source.lastHopToken)
            },
            repeatSourcesTruncated = repeatSourcesTruncated,
        )

    private fun validMessageBase(
        messageId: String,
        timestamp: Long,
        inbound: Boolean,
        kind: String,
        authenticated: Boolean,
        peerId: String?,
        channelSlot: Int?,
        senderName: String,
        text: String,
        state: String,
    ): Boolean {
        if (!validUint32(messageId) || timestamp !in 0L..UINT32_MAX ||
            state !in MESSAGE_STATES || text.toByteArray(Charsets.UTF_8).size !in 1..160 ||
            senderName.toByteArray(Charsets.UTF_8).size > 32 ||
            (inbound && state != "received") || (!inbound && state == "received")
        ) return false
        return when (kind) {
            "direct" -> authenticated && peerId != null &&
                MeshPeerKeyPolicy.isCanonicalBase64Url(peerId) && channelSlot == null
            "channel" -> !authenticated && peerId == null && channelSlot in 0..3 &&
                state !in setOf("delivered", "unconfirmed")
            else -> false
        }
    }

    private fun validMessageV2(item: FirmwareMessageV2): Boolean {
        if (!validMessageBase(
                item.messageId, item.timestamp, item.inbound, item.kind,
                item.authenticated, item.peerId, item.channelSlot,
                item.senderName, item.text, item.state,
            ) || !validUint32(item.revision) || item.route !in MESSAGE_ROUTES ||
            item.localTx !in LOCAL_TX_STATES || item.deliveryAck !in DELIVERY_ACK_STATES ||
            item.repeatersHeard != null || item.repeaterCount?.let { it !in 0..63 } == true ||
            item.rssiDbm?.isFinite() == false || item.snrDb?.isFinite() == false ||
            (item.kind == "channel" && item.route != "flood")
        ) return false

        if (item.inbound) {
            val validRepeaterEvidence = when (item.route) {
                "flood" -> item.repeaterCount != null
                "direct" -> item.repeaterCount == null
                else -> false
            }
            return item.localTx == "not_applicable" &&
                item.deliveryAck == "not_applicable" && validRepeaterEvidence
        }
        if (item.unread || item.rssiDbm != null || item.snrDb != null) return false
        return when (item.state) {
            "queued" -> item.localTx == "pending" && item.deliveryAck == "not_applicable" &&
                item.repeaterCount == null
            "sent" -> item.localTx == "sent" &&
                item.deliveryAck == (if (item.kind == "direct") "pending" else "not_applicable") &&
                item.repeaterCount == null
            "delivered" -> item.kind == "direct" && item.localTx == "sent" &&
                item.deliveryAck == "received"
            "unconfirmed" -> item.kind == "direct" && item.localTx == "sent" &&
                item.deliveryAck == "not_received" && item.repeaterCount == null
            "failed" -> item.localTx == "failed" && item.deliveryAck == "not_applicable" &&
                item.repeaterCount == null
            "cancelled" -> item.localTx == "cancelled" && item.deliveryAck == "not_applicable" &&
                item.repeaterCount == null
            else -> false
        }
    }

    private fun validMessageV3(item: FirmwareMessageV3): Boolean =
        validMessageV2(item.v2Shape()) && item.repeatCount?.let { it in 0..255 } != false &&
            validLocalRepeatEvidence(item)

    private fun FirmwareMessageV3.v2Shape() = FirmwareMessageV2(
        messageId = messageId,
        revision = revision,
        timestamp = timestamp,
        inbound = inbound,
        kind = kind,
        peerId = peerId,
        channelSlot = channelSlot,
        authenticated = authenticated,
        unread = unread,
        senderName = senderName,
        text = text,
        state = state,
        route = route,
        localTx = localTx,
        deliveryAck = deliveryAck,
        repeaterCount = repeaterCount,
        repeatersHeard = repeatersHeard,
        rssiDbm = rssiDbm,
        snrDb = snrDb,
    )

    private fun FirmwareMessageV4.v3Shape() = FirmwareMessageV3(
        messageId = messageId,
        revision = revision,
        timestamp = timestamp,
        inbound = inbound,
        kind = kind,
        peerId = peerId,
        channelSlot = channelSlot,
        authenticated = authenticated,
        unread = unread,
        senderName = senderName,
        text = text,
        state = state,
        route = route,
        localTx = localTx,
        deliveryAck = deliveryAck,
        repeaterCount = repeaterCount,
        repeatersHeard = repeatersHeard,
        repeatCount = repeatCount,
        rssiDbm = rssiDbm,
        snrDb = snrDb,
    )

    /**
     * A repeat count is a local observation of matching rebroadcast packet copies,
     * not a route relay count, fan-out count, or recipient delivery acknowledgement.
     */
    private fun validLocalRepeatEvidence(item: FirmwareMessageV3): Boolean {
        if (item.repeatCount == null) return true
        return !item.inbound && item.kind == "channel" && item.state == "sent" &&
            item.route == "flood" && item.localTx == "sent" &&
            item.deliveryAck == "not_applicable" && item.repeaterCount == null &&
            item.repeatersHeard == null
    }

    private fun validMessageV4(item: FirmwareMessageV4): Boolean {
        if (!validMessageV3(item.v3Shape())) return false
        val isSentChannel = !item.inbound && item.kind == "channel" && item.state == "sent"
        return if (isSentChannel) {
            val unavailable = item.repeatCount == null && item.repeatObservationOpen == null &&
                item.repeatSources == null && item.repeatSourcesTruncated == null
            val available = item.repeatCount != null && item.repeatObservationOpen != null &&
                item.repeatSources != null && item.repeatSourcesTruncated != null &&
                item.repeatSources.size <= 4 &&
                item.repeatSources.size <= item.repeatCount &&
                item.repeatSources.map(FirmwareRepeatSource::lastHopToken).distinct().size ==
                    item.repeatSources.size &&
                item.repeatSources.all(::validRepeatSource) &&
                (!item.repeatSourcesTruncated ||
                    (item.repeatSources.size == 4 && item.repeatCount >= 5))
            unavailable || available
        } else {
            item.repeatCount == null && item.repeatObservationOpen == null &&
                item.repeatSources == null && item.repeatSourcesTruncated == null
        }
    }

    private fun validRepeatSource(source: FirmwareRepeatSource): Boolean =
        REPEAT_LAST_HOP_TOKEN.matches(source.lastHopToken)

    private fun hasExactV3MessageItemKeys(wire: String): Boolean = try {
        hasExactMessageItemKeys(wire, MESSAGE_V3_ITEM_KEYS)
    } catch (_: Throwable) {
        false
    }

    private fun hasExactMessageItemKeys(wire: String, expectedKeys: Set<String>): Boolean = try {
        strictJson.parseToJsonElement(wire).jsonObject["items"]?.jsonArray?.all { item ->
            item.jsonObject.keys == expectedKeys
        } == true
    } catch (_: Throwable) {
        false
    }

    private fun validUint32(value: String, allowZero: Boolean = false): Boolean =
        value.toLongOrNull()?.let {
            it in (if (allowZero) 0L else 1L)..UINT32_MAX && value == it.toString()
        } == true

    fun channels(payload: ByteArray, expectedProtocolVersion: Int = 1): List<MeshChannel> {
        val expectedSchema = when (expectedProtocolVersion) {
            1 -> "kitsu.channels.v1"
            2 -> "kitsu.channels.v2"
            else -> throw TransportException("unsupported_protocol")
        }
        val actualSchema = try {
            strictJson.parseToJsonElement(payload.toString(Charsets.UTF_8))
                .jsonObject["schema"]?.jsonPrimitive?.content
        } catch (failure: Throwable) {
            throw TransportException("malformed_channels", failure)
        }
        if (actualSchema == null) throw TransportException("malformed_channels")
        requireSchema(actualSchema, expectedSchema)
        return if (expectedProtocolVersion == 1) channelsV1(payload) else channelsV2(payload)
    }

    private fun channelsV1(payload: ByteArray): List<MeshChannel> {
        val wire = payload.toString(Charsets.UTF_8)
        val value = decodeStrict<FirmwareChannelPageV1>(wire, "malformed_channels")
        requireSchema(value.schema, "kitsu.channels.v1")
        if (!hasExactChannelItemKeys(wire, CHANNEL_V1_ITEM_KEYS) ||
            value.items.size != 4 || value.items.map { it.slot } != listOf(0, 1, 2, 3) ||
            value.items.any { !validChannelName(it.configured, it.name) }
        ) throw TransportException("malformed_channels")
        return value.items.map { MeshChannel(it.slot, it.configured, it.name) }
    }

    private fun channelsV2(payload: ByteArray): List<MeshChannel> {
        val wire = payload.toString(Charsets.UTF_8)
        val value = decodeStrict<FirmwareChannelPageV2>(wire, "malformed_channels")
        requireSchema(value.schema, "kitsu.channels.v2")
        if (!hasExactChannelItemKeys(wire, CHANNEL_V2_ITEM_KEYS) ||
            value.items.size != 4 || value.items.map { it.slot } != listOf(0, 1, 2, 3) ||
            value.items.any {
                !validChannelName(it.configured, it.name) ||
                    !validChannelRegionScope(it.slot, it.configured, it.name, it.regionScope)
            }
        ) throw TransportException("malformed_channels")
        return value.items.map {
            MeshChannel(
                slot = it.slot,
                configured = it.configured,
                name = it.name,
                regionScope = if (it.regionScope == "EU") ChannelRegionScope.EU else null,
            )
        }
    }

    fun meshConfiguration(payload: ByteArray): MeshConfigurationReceipt {
        val value = decode<FirmwareMeshConfiguration>(payload, "malformed_mesh_configuration")
        requireSchema(value.schema, "kitsu.mesh-config.v1")
        if (value.profile != "uk_eu_narrow" || value.txPowerDbm != 22) {
            throw TransportException("unexpected_mesh_profile")
        }
        return MeshConfigurationReceipt(value.enabled, value.profile, value.txPowerDbm)
    }

    fun controllerForget(payload: ByteArray): ControllerForgetReceipt {
        val wire = payload.toString(Charsets.UTF_8)
        val keys = try {
            strictJson.parseToJsonElement(wire).jsonObject.keys
        } catch (failure: Throwable) {
            throw TransportException("malformed_controller_forget", failure)
        }
        val value = try {
            strictJson.decodeFromString<ControllerForgetReceipt>(wire)
        } catch (failure: Throwable) {
            throw TransportException("malformed_controller_forget", failure)
        }
        requireSchema(value.schema, "kitsu.controller-forget.v1")
        val expectedKeys = if (value.accepted) CONTROLLER_FORGET_ACCEPTED_KEYS else CONTROLLER_FORGET_REJECTED_KEYS
        if (keys != expectedKeys) throw TransportException("malformed_controller_forget")
        if (!value.accepted) {
            if (value.error != "storage_failed") throw TransportException("malformed_controller_forget")
            throw TransportException(value.error)
        }
        if (value.error != null) throw TransportException("malformed_controller_forget")
        return value
    }

    fun action(payload: ByteArray, command: ActionCommand): ActionReceipt {
        val wire = payload.toString(Charsets.UTF_8)
        val root = try {
            strictJson.parseToJsonElement(wire).jsonObject
        } catch (failure: Throwable) {
            throw TransportException("malformed_action_receipt", failure)
        }
        val receipt = try {
            strictJson.decodeFromString<ActionReceipt>(wire)
        } catch (failure: Throwable) {
            throw TransportException("malformed_action_receipt", failure)
        }
        val expectedKeys = if (receipt.accepted) ACTION_ACCEPTED_KEYS else ACTION_REJECTED_KEYS
        if (root.keys != expectedKeys || receipt.clientRequestId != command.clientRequestId) {
            throw TransportException("malformed_action_receipt")
        }
        if (!receipt.accepted) {
            val error = receipt.errorCode
            if (receipt.state != "rejected" || error == null || !ACTION_TOKEN.matches(error)) {
                throw TransportException("malformed_action_receipt")
            }
            throw TransportException(error)
        }
        val expectedState = if (command.kind in setOf(ActionKind.SEND_MESSAGE, ActionKind.ADVERTISE_ONCE)) {
            "queued"
        } else {
            "applied"
        }
        if (receipt.state != expectedState || receipt.errorCode != null) {
            throw TransportException("malformed_action_receipt")
        }
        return receipt
    }

    fun firmwareUpdate(payload: ByteArray): FirmwareUpdateReceipt {
        val wire = payload.toString(Charsets.UTF_8)
        val keys = try {
            strictJson.parseToJsonElement(wire).jsonObject.keys
        } catch (failure: Throwable) {
            throw TransportException("malformed_firmware_update_receipt", failure)
        }
        if (keys != UPDATE_RECEIPT_KEYS) {
            throw TransportException("malformed_firmware_update_receipt")
        }
        val value = try {
            strictJson.decodeFromString<FirmwareUpdateReceipt>(wire)
        } catch (failure: Throwable) {
            throw TransportException("malformed_firmware_update_receipt", failure)
        }
        if (value.protocol != 1 || value.state !in UPDATE_STATES || value.imageBytes < 0 ||
            value.nextOffset !in 0..value.imageBytes ||
            value.chunkBytes != FirmwareUpdatePackageReader.CHUNK_BYTES ||
            (value.updateId != null && !LOWER_SHA.matches(value.updateId))
        ) throw TransportException("malformed_firmware_update_receipt")
        if (!value.ok) {
            if (value.error == null) throw TransportException("malformed_firmware_update_receipt")
            throw TransportException(value.error)
        }
        if ((value.state == "failed") != (value.error != null)) {
            throw TransportException("malformed_firmware_update_receipt")
        }
        return value
    }

    private inline fun <reified T> decode(payload: ByteArray, code: String): T = try {
        json.decodeFromString(payload.toString(Charsets.UTF_8))
    } catch (failure: Throwable) {
        throw TransportException(code, failure)
    }

    private inline fun <reified T> decodeStrict(wire: String, code: String): T = try {
        strictJson.decodeFromString(wire)
    } catch (failure: Throwable) {
        throw TransportException(code, failure)
    }

    private fun requireSchema(actual: String, expected: String) {
        if (actual != expected) throw TransportException("unsupported_protocol")
    }

    private fun validChannelName(configured: Boolean, name: String?): Boolean = when {
        !configured -> name == null
        name == null -> false
        name.toByteArray(Charsets.UTF_8).size !in 1..32 -> false
        name.any { it.isISOControl() || Character.isSurrogate(it) } -> false
        else -> true
    }

    private fun validChannelRegionScope(
        slot: Int,
        configured: Boolean,
        name: String?,
        regionScope: String?,
    ): Boolean {
        if (slot == 0) return configured && name == "Public" && regionScope == null
        return regionScope == null || configured && regionScope == "EU"
    }

    private fun hasExactChannelItemKeys(wire: String, expectedKeys: Set<String>): Boolean = try {
        strictJson.parseToJsonElement(wire).jsonObject["items"]?.jsonArray?.all { item ->
            item.jsonObject.keys == expectedKeys
        } == true
    } catch (_: Throwable) {
        false
    }

    private val LOWER_SHA = Regex("^[0-9a-f]{64}$")
    private val ACTION_TOKEN = Regex("^[a-z][a-z0-9_]{0,47}$")
    private val MESSAGE_STATES = setOf(
        "received", "queued", "sent", "delivered", "unconfirmed", "failed", "cancelled",
    )
    private val MESSAGE_ROUTES = setOf("direct", "flood")
    private val LOCAL_TX_STATES = setOf("not_applicable", "pending", "sent", "failed", "cancelled")
    private val DELIVERY_ACK_STATES = setOf("not_applicable", "pending", "received", "not_received")
    private val MESSAGE_READ_ERRORS = setOf(
        "request_rejected",
        "journal_session_mismatch",
        "snapshot_changed",
        "message_not_inbound",
    )
    private val MESSAGE_V3_ITEM_KEYS = setOf(
        "message_id", "revision", "timestamp", "inbound", "kind", "peer_id",
        "channel_slot", "authenticated", "unread", "sender_name", "text", "state",
        "route", "local_tx", "delivery_ack", "repeater_count", "repeaters_heard",
        "repeat_count", "rssi_dbm", "snr_db",
    )
    private val MESSAGE_V4_ITEM_KEYS = MESSAGE_V3_ITEM_KEYS + setOf(
        "repeat_observation_open", "repeat_sources", "repeat_sources_truncated",
    )
    private val REPEAT_LAST_HOP_TOKEN = Regex("^(?:[0-9A-F]{2}){1,3}$")
    private val CHANNEL_V1_ITEM_KEYS = setOf("slot", "configured", "name")
    private val CHANNEL_V2_ITEM_KEYS = CHANNEL_V1_ITEM_KEYS + "region_scope"
    private const val UINT32_MAX = 0xffff_ffffL
    private const val MIN_VALID_MESH_EPOCH = 1_704_067_200L
    private const val MAX_VALID_MESH_EPOCH = 4_102_444_800L
    private val LAST_FLOOD_ADVERT_KEYS = setOf(
        "emitted_at", "state", "repeat_count", "observation_open",
    )
    private val LAST_NEARBY_ADVERT_KEYS = LAST_FLOOD_ADVERT_KEYS
    private val LAST_FLOOD_ADVERT_V2_KEYS = LAST_FLOOD_ADVERT_KEYS + setOf(
        "repeat_sources", "repeat_sources_truncated",
    )
    private val ADVERT_LIFECYCLE_STATES = setOf("queued", "sent", "tx_failed")
    private val ADVERTISE_ERRORS = setOf(
        "companion_unavailable",
        "mesh_identity_unavailable",
        "mesh_disabled",
        "mesh_radio_unavailable",
        "time_unset",
        "tx_policy_locked",
        "advertise_cooldown",
        "send_busy",
        "queue_full",
        "location_unavailable",
        "idempotency_unavailable",
        "action_expired",
        "invalid_expiry",
        "action_id_conflict",
        "action_result_unknown",
        "invalid_params",
    )
    private val CONTROLLER_FORGET_ACCEPTED_KEYS = setOf("schema", "accepted")
    private val CONTROLLER_FORGET_REJECTED_KEYS = CONTROLLER_FORGET_ACCEPTED_KEYS + "error"
    private val ACTION_ACCEPTED_KEYS = setOf("action_id", "accepted", "state")
    private val ACTION_REJECTED_KEYS = ACTION_ACCEPTED_KEYS + "error_code"
    private val UPDATE_RECEIPT_KEYS = setOf(
        "ok", "protocol", "state", "update_id", "firmware_version", "image_bytes",
        "next_offset", "chunk_bytes", "resumed", "replayed", "scheduled", "error",
    )
    private val UPDATE_STATES = setOf(
        "idle", "receiving", "ready_to_reboot", "pending_verify", "confirmed", "rolled_back", "failed",
    )
}
