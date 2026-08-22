package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.ControllerForgetReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.MeshPeerKeyPolicy
import app.kitsu.mobile.model.MeshState
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.NeedLevels
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.WIRE_VERSION
import app.kitsu.mobile.update.FirmwareUpdatePackageReader
import app.kitsu.mobile.update.FirmwareUpdateReceipt
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.booleanOrNull
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
        @SerialName("event_count") val eventCount: Int = 0,
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

    @Serializable private data class FirmwareMessage(
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

    @Serializable private data class FirmwareMessagePage(
        val schema: String,
        val items: List<FirmwareMessage>,
        val cursor: String? = null,
        @SerialName("has_more") val hasMore: Boolean,
        val gap: Boolean,
    )

    @Serializable private data class FirmwareChannel(val slot: Int, val configured: Boolean, val name: String? = null)
    @Serializable private data class FirmwareChannelPage(val schema: String, val items: List<FirmwareChannel>)
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
        val value = decode<FirmwareState>(payload, "malformed_state")
        requireSchema(value.schema, "kitsu.state.v1")
        if (value.energy !in 0..100 || value.curiosity !in 0..100 || value.affection !in 0..100 ||
            (value.batteryPercent != null && value.batteryPercent !in 0..100) ||
            (value.batteryMillivolts != null && value.batteryMillivolts < 0) ||
            (value.packId != null && value.packId !in 0..0xffffffffL) ||
            (value.packRevision != null && value.packRevision !in 0..0xffffffffL) ||
            value.bondLevel < 0 || value.bondExperience < 0 || value.bondProgressPercent !in 0..100 ||
            (value.appearanceVariant != null && value.appearanceVariant !in 0..255) ||
            value.unlockMask < 0 || value.memoryCount < 0
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
            ),
            cursor = value.eventCount.takeIf { it > 0 }?.toString(),
            updatedAt = observedAtEpochSeconds,
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

    fun messages(payload: ByteArray): MessagePage {
        val value = decode<FirmwareMessagePage>(payload, "malformed_messages")
        requireSchema(value.schema, "kitsu.messages.v1")
        if (value.items.any { item ->
                item.state !in MESSAGE_STATES || when (item.kind) {
                    "direct" -> item.peerId == null ||
                        !MeshPeerKeyPolicy.isCanonicalBase64Url(item.peerId) ||
                        item.channelSlot != null
                    "channel" -> item.peerId != null || item.channelSlot !in 0..3
                    else -> true
                }
            }) throw TransportException("malformed_messages")
        return MessagePage(
            items = value.items.map { item ->
                Message(
                    id = item.messageId,
                    cursor = item.messageId,
                    direction = if (item.inbound) "inbound" else "outbound",
                    peerId = if (item.kind == "direct") item.peerId else
                        item.senderName.takeIf { it.isNotBlank() }?.let { "$it (unverified)" },
                    channel = item.channelSlot?.toString(),
                    text = item.text,
                    state = item.state,
                    occurredAt = item.timestamp,
                )
            },
            cursor = value.cursor,
            hasMore = value.hasMore,
            cursorExpired = value.gap,
        )
    }

    fun channels(payload: ByteArray): List<MeshChannel> {
        val value = decode<FirmwareChannelPage>(payload, "malformed_channels")
        requireSchema(value.schema, "kitsu.channels.v1")
        if (value.items.size != 4 || value.items.map { it.slot } != listOf(0, 1, 2, 3) ||
            value.items.any { !validChannelName(it.configured, it.name) }
        ) throw TransportException("malformed_channels")
        return value.items.map { MeshChannel(it.slot, it.configured, it.name) }
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
        val expectedState = if (command.kind == ActionKind.SEND_MESSAGE) "queued" else "applied"
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

    private val LOWER_SHA = Regex("^[0-9a-f]{64}$")
    private val ACTION_TOKEN = Regex("^[a-z][a-z0-9_]{0,47}$")
    private val MESSAGE_STATES = setOf(
        "received", "queued", "sent", "delivered", "unconfirmed", "failed", "cancelled",
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
