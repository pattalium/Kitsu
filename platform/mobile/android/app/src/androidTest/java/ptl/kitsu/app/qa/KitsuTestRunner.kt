package ptl.kitsu.app.qa

import android.app.Application
import android.content.Context
import androidx.test.runner.AndroidJUnitRunner
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import ptl.kitsu.app.KitsuApplication
import ptl.kitsu.app.KitsuServiceContainer
import ptl.kitsu.app.cache.EncryptedBoundedCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.connection.InMemoryReconnectSuppressionStore
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.ChannelRegionScope
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.ENCOUNTER_CODES_SCHEMA
import ptl.kitsu.app.model.EncounterCodePage
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageMarkReadReceipt
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.MeshState
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.model.RepeatSource
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.repository.OwnerRepository
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.transport.FirmwareBlePayloadMapper
import ptl.kitsu.app.transport.FirmwareMessageApiPolicy
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.TransportException
import ptl.kitsu.app.update.FirmwareUpdateReceipt

/** Installs a no-radio, no-network fixture only inside the instrumentation APK process. */
class KitsuTestRunner : AndroidJUnitRunner() {
    override fun newApplication(classLoader: ClassLoader, className: String, context: Context): Application =
        super.newApplication(classLoader, KitsuFixtureApplication::class.java.name, context)
}

class KitsuFixtureApplication : KitsuApplication() {
    override fun createServices(): KitsuServiceContainer {
        val credentials = FixtureCredentialStore()
        val transport = FixtureTransport()
        return object : KitsuServiceContainer {
            override val ownerRepository = OwnerRepository(
                coordinator = ConnectionCoordinator(
                    transport,
                    InMemoryReconnectSuppressionStore(initialValue = false),
                ),
                cache = EncryptedBoundedCache(this@KitsuFixtureApplication),
                credentials = credentials,
                pairingService = FixturePairingService(credentials),
            )
        }
    }
}

/**
 * Canonical firmware-0.16.1 visual fixture state.
 *
 * The real messages.get.v4 journal is boot-scoped, mutable, and paged. Keeping the
 * fixture equally strict prevents screenshots from silently exercising legacy v1
 * fallback or impossible delivery/read combinations.
 */
object FixtureScenario {
    const val FIRMWARE_VERSION = "0.16.5-fixture"
    const val JOURNAL_SESSION = "15"
    const val DIRECT_PEER_KEY = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    const val MANUAL_PEER_KEY = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"

    private val lock = Any()
    private var journalRevision = 500L
    private var rows = initialMessages()
    private var lastFloodAdvert: LastFloodAdvert? = initialLastFloodAdvert()
    private var lastNearbyAdvert: LastNearbyAdvert? = null
    private var eventSequence = 800L
    private val events = MutableSharedFlow<EventEnvelope>(extraBufferCapacity = 16)

    @Volatile
    private var messagesFailureCode: String? = null

    @Volatile
    var meshEnabled: Boolean = true
        private set

    @Volatile
    var advertiseQueued: Boolean = false
        private set

    @Volatile
    var lastMessageOperation: String? = null
        private set

    fun reset() = synchronized(lock) {
        journalRevision = 500L
        rows = initialMessages()
        lastFloodAdvert = initialLastFloodAdvert()
        lastNearbyAdvert = null
        eventSequence = 800L
        messagesFailureCode = null
        meshEnabled = true
        advertiseQueued = false
        lastMessageOperation = null
    }

    fun failMessagesWith(code: String?) {
        messagesFailureCode = code
    }

    fun setMeshEnabled(enabled: Boolean) {
        meshEnabled = enabled
    }

    fun clearAdvertisementRecords() = synchronized(lock) {
        lastFloodAdvert = null
        lastNearbyAdvert = null
    }

    fun recordAdvertisement(scope: AdvertiseScope) = synchronized(lock) {
        advertiseQueued = true
        when (scope) {
            AdvertiseScope.NEARBY -> {
                lastNearbyAdvert = LastNearbyAdvert(
                    emittedAt = 1_787_350_401,
                    state = "queued",
                )
            }
            AdvertiseScope.MESH -> {
                lastFloodAdvert = LastFloodAdvert(
                    emittedAt = 1_787_350_401,
                    state = "queued",
                    repeatCount = null,
                    observationOpen = false,
                )
            }
        }
        emitRefreshLocked()
    }

    internal fun lastFloodAdvert(): LastFloodAdvert? = synchronized(lock) { lastFloodAdvert }
    internal fun lastNearbyAdvert(): LastNearbyAdvert? = synchronized(lock) { lastNearbyAdvert }
    internal fun events(): Flow<EventEnvelope> = events

    fun observeChannelRepeat() = synchronized(lock) {
        journalRevision += 1L
        val revision = journalRevision.toString()
        rows = rows.map { message ->
            if (message.id == "104") {
                message.copy(
                    revision = revision,
                    repeatCount = 1,
                    repeatObservationOpen = true,
                    repeatSources = listOf(RepeatSource("00")),
                    repeatSourcesTruncated = false,
                )
            } else {
                message
            }
        }
        emitRefreshLocked()
    }

    private fun emitRefreshLocked() {
        eventSequence += 1L
        check(
            events.tryEmit(
                EventEnvelope(
                    v = 1,
                    cursor = "ble:$eventSequence",
                    kind = "refresh",
                    body = buildJsonObject {},
                ),
            ),
        ) { "fixture_event_buffer_full" }
    }

    internal fun recordMessageOperation(operation: String) {
        lastMessageOperation = operation
    }

    internal fun snapshot(): FixtureMessageSnapshot = synchronized(lock) {
        messagesFailureCode?.let { throw TransportException(it) }
        FixtureMessageSnapshot(rows.toList(), journalRevision.toString())
    }

    internal fun markRead(
        journalSession: String,
        messageIds: List<String>,
    ): MessageMarkReadReceipt = synchronized(lock) {
        if (journalSession != JOURNAL_SESSION) throw TransportException("journal_session_mismatch")
        if (messageIds.isEmpty() || messageIds.size > 24 || messageIds.distinct().size != messageIds.size) {
            throw TransportException("message_read_batch_invalid")
        }
        if (messageIds.any { id ->
                id.toLongOrNull()?.let { value ->
                    value !in 1L..0xffff_ffffL || id != value.toString()
                } != false
            }
        ) {
            throw TransportException("message_read_id_invalid")
        }
        val selected = rows.filter { it.id in messageIds }
        if (selected.size != messageIds.size) throw TransportException("message_read_target_stale")
        if (selected.any { !it.direction.equals("inbound", ignoreCase = true) }) {
            throw TransportException("message_not_inbound")
        }

        val changed = selected.count { it.unreadOnKitsu == true }
        val unchanged = selected.size - changed
        if (changed > 0) {
            journalRevision += 1L
            val revision = journalRevision.toString()
            rows = rows.map { message ->
                if (message.id in messageIds && message.unreadOnKitsu == true) {
                    message.copy(unreadOnKitsu = false, revision = revision)
                } else {
                    message
                }
            }
        }
        MessageMarkReadReceipt(
            schema = "kitsu.messages-mark-read.v1",
            accepted = true,
            error = null,
            markedCount = changed,
            unchangedCount = unchanged,
            journalSession = JOURNAL_SESSION,
            journalRevision = journalRevision.toString(),
        )
    }

    private fun initialMessages(): List<Message> = listOf(
        Message(
            id = "101",
            cursor = "101",
            direction = "inbound",
            peerId = DIRECT_PEER_KEY,
            text = "Found a quiet trail nearby.",
            state = "received",
            occurredAt = 1_787_054_400,
            revision = "201",
            journalSession = JOURNAL_SESSION,
            senderName = "Copper Fox",
            unreadOnKitsu = true,
            route = "direct",
            localTx = "not_applicable",
            deliveryAck = "not_applicable",
            rssiDbm = -58.0,
            snrDb = 11.2,
        ),
        Message(
            id = "102",
            cursor = "102",
            direction = "outbound",
            peerId = DIRECT_PEER_KEY,
            text = "Meet by the old oak?",
            state = "delivered",
            occurredAt = 1_787_054_700,
            revision = "202",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "direct",
            localTx = "sent",
            deliveryAck = "received",
            repeaterCount = 0,
        ),
        Message(
            id = "103",
            cursor = "103",
            direction = "inbound",
            channel = "0",
            text = "Weather is clear tonight.",
            state = "received",
            occurredAt = 1_787_055_000,
            revision = "203",
            journalSession = JOURNAL_SESSION,
            senderName = "Shade",
            unreadOnKitsu = true,
            route = "flood",
            localTx = "not_applicable",
            deliveryAck = "not_applicable",
            repeaterCount = 2,
            rssiDbm = -71.0,
            snrDb = 7.5,
        ),
        Message(
            id = "104",
            cursor = "104",
            direction = "outbound",
            channel = "0",
            text = "Camp channel check-in.",
            state = "sent",
            occurredAt = 1_787_055_300,
            revision = "204",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "flood",
            localTx = "sent",
            deliveryAck = "not_applicable",
            repeatCount = 0,
            repeatObservationOpen = true,
            repeatSources = emptyList(),
            repeatSourcesTruncated = false,
        ),
        Message(
            id = "105",
            cursor = "105",
            direction = "outbound",
            peerId = DIRECT_PEER_KEY,
            text = "Did that reach you?",
            state = "unconfirmed",
            occurredAt = 1_787_227_200,
            revision = "205",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "flood",
            localTx = "sent",
            deliveryAck = "not_received",
        ),
        Message(
            id = "106",
            cursor = "106",
            direction = "inbound",
            peerId = DIRECT_PEER_KEY,
            text = "Yes. The ridge blocked the first reply.",
            state = "received",
            occurredAt = 1_787_400_000,
            revision = "206",
            journalSession = JOURNAL_SESSION,
            senderName = "Copper Fox",
            unreadOnKitsu = false,
            route = "direct",
            localTx = "not_applicable",
            deliveryAck = "not_applicable",
            rssiDbm = -64.0,
            snrDb = 9.0,
        ),
        Message(
            id = "107",
            cursor = "107",
            direction = "outbound",
            peerId = DIRECT_PEER_KEY,
            text = "Trying the valley route.",
            state = "failed",
            occurredAt = 1_787_400_300,
            revision = "207",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "flood",
            localTx = "failed",
            deliveryAck = "not_applicable",
        ),
        Message(
            id = "108",
            cursor = "108",
            direction = "inbound",
            channel = "0",
            text = "Morning net starts in ten minutes.",
            state = "received",
            occurredAt = 1_787_479_200,
            revision = "208",
            journalSession = JOURNAL_SESSION,
            senderName = "Shade",
            unreadOnKitsu = false,
            route = "flood",
            localTx = "not_applicable",
            deliveryAck = "not_applicable",
            repeaterCount = 1,
            rssiDbm = -69.0,
            snrDb = 8.0,
        ),
        Message(
            id = "109",
            cursor = "109",
            direction = "outbound",
            peerId = DIRECT_PEER_KEY,
            text = "Packing now.",
            state = "queued",
            occurredAt = 1_787_479_500,
            revision = "209",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "flood",
            localTx = "pending",
            deliveryAck = "not_applicable",
        ),
        Message(
            id = "110",
            cursor = "110",
            direction = "outbound",
            peerId = DIRECT_PEER_KEY,
            text = "Meet at sunset.",
            state = "delivered",
            occurredAt = 1_787_479_800,
            revision = "210",
            journalSession = JOURNAL_SESSION,
            unreadOnKitsu = false,
            route = "flood",
            localTx = "sent",
            deliveryAck = "received",
            repeaterCount = 1,
        ),
    )

    private fun initialLastFloodAdvert() = LastFloodAdvert(
        emittedAt = 1_787_350_300,
        state = "sent",
        repeatCount = 1,
        observationOpen = true,
        repeatSources = listOf(RepeatSource("00")),
        repeatSourcesTruncated = false,
    )
}

internal data class FixtureMessageSnapshot(
    val rows: List<Message>,
    val journalRevision: String,
)

private class FixtureCredentialStore : CredentialStore {
    val fixture = BondedCompanion(
        deviceAddress = "02:00:00:00:00:32",
        displayName = "Pocket Kitsu",
        controllerIdB64 = "AAAAAAAAAAAAAAAAAAAAAA",
        controllerRootB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    )
    private var active: BondedCompanion? = fixture
    private var pending: BondedCompanion? = null
    private var pendingForget: String? = null

    override suspend fun bondedCompanion(): BondedCompanion? = active
    override suspend fun bondedCompanions(): List<BondedCompanion> = listOfNotNull(active)
    override suspend fun saveBondedCompanion(value: BondedCompanion?) { active = value }
    override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? =
        active?.takeIf { it.deviceAddress.equals(deviceAddress, true) }
    override suspend fun removeBondedCompanion(deviceAddress: String): Boolean =
        active?.deviceAddress?.equals(deviceAddress, true)?.also { if (it) active = null } ?: false
    override suspend fun pendingBondedCompanion(): BondedCompanion? = pending
    override suspend fun savePendingBondedCompanion(value: BondedCompanion?) { pending = value }
    override suspend fun pendingControllerForgetAddress(): String? = pendingForget
    override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) { pendingForget = deviceAddress }
}

private class FixturePairingService(
    private val credentials: FixtureCredentialStore,
) : ControllerPairingService {
    override suspend fun pairController(
        label: String,
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion = credentials.fixture.copy(displayName = label).also {
        credentials.saveBondedCompanion(it)
    }

    override suspend fun finishPendingPairing(
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion = credentials.fixture.also {
        credentials.saveBondedCompanion(it)
    }

    override suspend fun repairBluetoothPairing(
        deviceAddress: String,
        onProgress: (BluetoothPairingRepairProgress) -> Unit,
    ): BondedCompanion = credentials.fixture

    override fun cancelPairing() = Unit
}

private class FixtureTransport : KitsuTransport {
    override val mode = ConnectionMode.DIRECT_BLE
    private var connected = false

    override fun isConnectedTo(deviceAddress: String): Boolean =
        connected && deviceAddress.equals("02:00:00:00:00:32", true)

    override suspend fun connect(): ConnectResult = ConnectResult.Connected.also { connected = true }
    override suspend fun disconnect() { connected = false }
    override suspend fun synchronizeClock() = Unit
    override suspend fun status() = KitsuStatus(
        deviceId = "KT32C0FFEE",
        companionName = "Kitsu",
        firmwareVersion = FixtureScenario.FIRMWARE_VERSION,
        listening = true,
        mood = "CURIOUS",
        batteryPercent = 84,
        batteryMillivolts = 4_012,
        packReady = true,
        packId = "local-fox",
        packRevision = 7,
        bondLevel = 8,
        bondExperience = 1_284,
        bondProgressPercent = 68,
        evolutionStage = "companion",
        appearanceVariant = "copper",
        personality = "curious",
        unlockMask = 15,
        memoryCount = 12,
        needs = NeedLevels(energy = 76, curiosity = 64, affection = 91),
        mesh = MeshState(
            enabled = FixtureScenario.meshEnabled,
            rxReady = FixtureScenario.meshEnabled,
            timeValid = true,
            oneShotReady = true,
            advertiseSupported = true,
            identityReady = true,
            advertiseReady = FixtureScenario.meshEnabled && !FixtureScenario.advertiseQueued,
            advertiseRetryAfterMs = if (FixtureScenario.advertiseQueued) 30_000 else 0,
            advertiseError = if (FixtureScenario.advertiseQueued) "advertise_cooldown" else null,
            lastFloodAdvert = FixtureScenario.lastFloodAdvert(),
            lastNearbyAdvert = FixtureScenario.lastNearbyAdvert(),
        ),
        cursor = "state-7",
        updatedAt = 1_787_350_400,
    )

    override suspend fun history(after: String?, limit: Int) = HistoryPage(
        items = listOf(
            HistoryEntry("history-1", "event-1", "bond", "Kitsu remembered your visit", 1_787_350_100),
            HistoryEntry("history-2", "event-2", "play", "You explored together", 1_787_350_200),
        ),
        cursor = "event-2",
    )

    override suspend fun peers() = PeerPage(
        listOf(
            Peer(
                FixtureScenario.DIRECT_PEER_KEY,
                "Copper Fox",
                lastHeardAt = 1_787_479_100,
                route = "direct",
            ),
        ),
    )

    override suspend fun messages(after: String?, limit: Int): MessagePage {
        val protocolVersion = FirmwareMessageApiPolicy.protocolVersion(FixtureScenario.FIRMWARE_VERSION)
        val operation = FirmwareMessageApiPolicy.operation(protocolVersion)
        check(protocolVersion == 4 && operation == "messages.get.v4")
        FixtureScenario.recordMessageOperation(operation)

        val snapshot = FixtureScenario.snapshot()
        val start = if (after == null) {
            0
        } else {
            val index = snapshot.rows.indexOfFirst { it.id == after }
            if (index < 0) {
                return strictV4Page(
                    rows = emptyList(),
                    cursor = null,
                    hasMore = false,
                    gap = true,
                    journalRevision = snapshot.journalRevision,
                )
            }
            index + 1
        }
        val boundedLimit = limit.coerceIn(1, 100)
        val page = snapshot.rows.drop(start).take(boundedLimit)
        return strictV4Page(
            rows = page,
            cursor = page.lastOrNull()?.id,
            hasMore = start + page.size < snapshot.rows.size,
            gap = false,
            journalRevision = snapshot.journalRevision,
        )
    }

    /** Runs fixture rows through the same strict v4 decoder used by authenticated BLE. */
    private fun strictV4Page(
        rows: List<Message>,
        cursor: String?,
        hasMore: Boolean,
        gap: Boolean,
        journalRevision: String,
    ): MessagePage {
        val payload = buildJsonObject {
            put("schema", "kitsu.messages.v4")
            put("journal_session", FixtureScenario.JOURNAL_SESSION)
            put("journal_revision", journalRevision)
            put("items", buildJsonArray { rows.forEach { add(it.toStrictV4Json()) } })
            put("cursor", cursor)
            put("has_more", hasMore)
            put("gap", gap)
        }.toString().toByteArray()
        return FirmwareBlePayloadMapper.messages(payload, expectedProtocolVersion = 4)
    }

    private fun Message.toStrictV4Json() = buildJsonObject {
        val inbound = direction.equals("inbound", ignoreCase = true)
        val channelSlot = channel?.toIntOrNull()
        put("message_id", id)
        put("revision", revision)
        put("timestamp", occurredAt)
        put("inbound", inbound)
        put("kind", if (channelSlot == null) "direct" else "channel")
        put("peer_id", peerId)
        put("channel_slot", channelSlot)
        put("authenticated", channelSlot == null)
        put("unread", unreadOnKitsu == true)
        put("sender_name", senderName)
        put("text", text)
        put("state", state)
        put("route", route)
        put("local_tx", localTx)
        put("delivery_ack", deliveryAck)
        put("repeater_count", repeaterCount)
        put("repeaters_heard", JsonNull)
        put("repeat_count", repeatCount)
        put("repeat_observation_open", repeatObservationOpen)
        put(
            "repeat_sources",
            repeatSources?.let { sources ->
                buildJsonArray {
                    sources.forEach { source ->
                        add(buildJsonObject { put("last_hop_token", source.lastHopToken) })
                    }
                }
            } ?: JsonNull,
        )
        put("repeat_sources_truncated", repeatSourcesTruncated)
        put("rssi_dbm", rssiDbm)
        put("snr_db", snrDb)
    }

    override suspend fun markMessagesRead(
        journalSession: String,
        messageIds: List<String>,
    ): MessageMarkReadReceipt = FixtureScenario.markRead(journalSession, messageIds)

    override suspend fun action(command: ActionCommand): ActionReceipt {
        if (command.kind == ActionKind.ADVERTISE_ONCE) {
            FixtureScenario.recordAdvertisement(requireNotNull(command.advertiseScope))
        }
        return ActionReceipt(
            clientRequestId = command.clientRequestId,
            accepted = true,
            state = if (command.kind in setOf(ActionKind.ADVERTISE_ONCE, ActionKind.SEND_MESSAGE)) "queued" else "applied",
        )
    }

    override fun events(after: String?): Flow<EventEnvelope> = FixtureScenario.events()
    override suspend fun channels(firmwareVersion: String?) = listOf(
        MeshChannel(0, true, "Camp", ChannelRegionScope.EU),
        MeshChannel(1, true, "Trail"),
    )
    override suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt {
        FixtureScenario.setMeshEnabled(enabled)
        return MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
    }
    override suspend fun encounterCodes(after: String?, limit: Int) =
        EncounterCodePage(ENCOUNTER_CODES_SCHEMA)
    override suspend fun forgetController() = ControllerForgetReceipt("kitsu.controller-forget.v1", true)
    override suspend fun firmwareUpdateStatus() = firmwareReceipt()
    override suspend fun beginFirmwareUpdate(manifest: ByteArray, signature: ByteArray) = firmwareReceipt()
    override suspend fun writeFirmwareUpdate(updateId: String, offset: Int, data: ByteArray) = firmwareReceipt()
    override suspend fun finishFirmwareUpdate(updateId: String) = firmwareReceipt()
    override suspend fun rebootFirmwareUpdate(updateId: String) = firmwareReceipt().copy(scheduled = true)
    override suspend fun abortFirmwareUpdate(updateId: String) = firmwareReceipt()

    private fun firmwareReceipt() = FirmwareUpdateReceipt(
        ok = true,
        protocol = 1,
        state = "idle",
        firmwareVersion = FixtureScenario.FIRMWARE_VERSION,
        imageBytes = 0,
        nextOffset = 0,
        chunkBytes = 4_096,
        resumed = false,
        replayed = false,
        scheduled = false,
    )

}
