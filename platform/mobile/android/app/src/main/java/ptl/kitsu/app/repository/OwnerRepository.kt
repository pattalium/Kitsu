package ptl.kitsu.app.repository

import ptl.kitsu.app.cache.CachePolicy
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.model.EncounterCatalogCreature
import ptl.kitsu.app.model.EncounterUnlockCode
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.NeighborInteractionReceipt
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.PartyJoinCommand
import ptl.kitsu.app.model.PartyRoundCommand
import ptl.kitsu.app.model.StoryTrigger
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.ControllerForgetPolicy
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.BluetoothPairingRepairStage
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.security.SafeLog
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.transport.FirmwareEncounterApiPolicy
import ptl.kitsu.app.transport.FirmwareFunApiPolicy
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.FirmwareMessageApiPolicy
import ptl.kitsu.app.transport.TransportException
import ptl.kitsu.app.update.FirmwareInstallProgress
import ptl.kitsu.app.update.FirmwareInstallStage
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import ptl.kitsu.app.update.VerifiedFirmwarePackage
import java.io.FileInputStream
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.conflate
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.yield

data class OwnerState(
    val connection: ConnectionState = ConnectionState(),
    val status: KitsuStatus? = null,
    val history: List<HistoryEntry> = emptyList(),
    val peers: List<Peer> = emptyList(),
    val encounterCatalog: List<EncounterCatalogCreature> = emptyList(),
    val encounterCatalogSupported: Boolean = false,
    val encounterCatalogErrorCode: String? = null,
    val nearbyKitsu: List<NearbyKitsu> = emptyList(),
    val nearbyKitsuSupported: Boolean = false,
    val nearbyInteractionKinds: Set<NeighborInteractionKind> = emptySet(),
    val nearbyKitsuErrorCode: String? = null,
    val funState: FunState? = null,
    val funSupported: Boolean = false,
    val funErrorCode: String? = null,
    val funMutationInFlight: Boolean = false,
    val messages: List<Message> = emptyList(),
    val messagesErrorCode: String? = null,
    val messageJournalSession: String? = null,
    val messageJournalRevision: String? = null,
    val messageProtocolVersion: Int = 1,
    val messageMarkReadSupported: Boolean = false,
    val messageReadErrorCode: String? = null,
    val channels: List<MeshChannel> = emptyList(),
    val savedKitsu: List<BondedCompanion> = emptyList(),
    val activeDeviceAddress: String? = null,
    val pendingPairing: BondedCompanion? = null,
    val pendingForgetAddress: String? = null,
    val loading: Boolean = false,
    val errorCode: String? = null,
    val pairing: Boolean = false,
    val pairingProgress: ControllerPairingProgress? = null,
    val repairingBluetoothPairing: Boolean = false,
    val bluetoothPairingRepairProgress: BluetoothPairingRepairProgress? = null,
    val meshConfigurationInFlight: Boolean = false,
    val meshAdvertisementInFlight: Boolean = false,
)

class OwnerRepository(
    private val coordinator: ConnectionCoordinator,
    private val cache: OwnerCache,
    private val credentials: CredentialStore,
    private val pairingService: ControllerPairingService,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate),
    private val advertiseSubmissionTimeoutMillis: Long = ADVERTISE_SUBMISSION_TIMEOUT_MILLIS,
) {
    private val mutableState = MutableStateFlow(OwnerState())
    val state: StateFlow<OwnerState> = mutableState.asStateFlow()
    private var eventJob: Job? = null
    private var lastLiveNamespace: OwnerCursorNamespace? = null
    private val refreshMutex = Mutex()
    private val messageRefreshMutex = Mutex()
    private val messageReadMutex = Mutex()
    private val initialHydration = CompletableDeferred<Unit>()

    init {
        require(advertiseSubmissionTimeoutMillis > 0L) { "advertise_timeout_required" }
        scope.launch {
            try {
                val cached = withContext(Dispatchers.IO) { cache.read() }
                val devices = credentials.bondedCompanions()
                val active = credentials.bondedCompanion()
                val pendingPairing = reconcilePendingPairing(devices)
                val pendingForget = reconcilePendingForget(devices)
                mutableState.value = mutableState.value.copy(
                    status = cached?.status,
                    history = cached?.history.orEmpty(),
                    peers = cached?.peers.orEmpty(),
                    channels = cached?.channels.orEmpty(),
                    messages = cached?.messages.orEmpty(),
                    savedKitsu = devices,
                    activeDeviceAddress = active?.deviceAddress,
                    pendingPairing = pendingPairing,
                    pendingForgetAddress = pendingForget,
                )
            } finally {
                // No live connect/refresh may race a late keystore-backed cache read.
                initialHydration.complete(Unit)
            }
        }
        scope.launch {
            coordinator.state.collect { connection ->
                mutableState.value = mutableState.value.copy(
                    connection = connection,
                    encounterCatalog = if (connection.connected) {
                        mutableState.value.encounterCatalog
                    } else emptyList(),
                    encounterCatalogSupported = connection.connected &&
                        mutableState.value.encounterCatalogSupported,
                    encounterCatalogErrorCode = if (connection.connected) {
                        mutableState.value.encounterCatalogErrorCode
                    } else null,
                    nearbyKitsu = if (connection.connected) {
                        mutableState.value.nearbyKitsu
                    } else emptyList(),
                    nearbyKitsuSupported = connection.connected &&
                        mutableState.value.nearbyKitsuSupported,
                    nearbyInteractionKinds = if (connection.connected) {
                        mutableState.value.nearbyInteractionKinds
                    } else emptySet(),
                    nearbyKitsuErrorCode = if (connection.connected) {
                        mutableState.value.nearbyKitsuErrorCode
                    } else null,
                    funState = if (connection.connected) mutableState.value.funState else null,
                    funSupported = connection.connected && mutableState.value.funSupported,
                    funErrorCode = if (connection.connected) mutableState.value.funErrorCode else null,
                    funMutationInFlight = connection.connected && mutableState.value.funMutationInFlight,
                    messageJournalSession = if (connection.connected) {
                        mutableState.value.messageJournalSession
                    } else null,
                    messageJournalRevision = if (connection.connected) {
                        mutableState.value.messageJournalRevision
                    } else null,
                    messageMarkReadSupported = if (connection.connected) {
                        mutableState.value.messageMarkReadSupported
                    } else false,
                )
            }
        }
    }

    suspend fun connectAndRefresh(userInitiated: Boolean = false) {
        initialHydration.await()
        if (mutableState.value.pairing || mutableState.value.repairingBluetoothPairing) return
        val selected = mutableState.value.activeDeviceAddress ?: credentials.bondedCompanion()?.deviceAddress
        if (selected == null) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = null)
            return
        }
        stopEvents()
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        val connection = coordinator.connect(userInitiated)
        if (!connection.connected) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = connection.detail)
            return
        }
        // Register the replay=0 notification collector before the first live read.
        // Otherwise an inbound packet arriving between refresh and subscription is
        // invisible until the firmware's next periodic refresh notification.
        OwnerConnectRefreshOrder.run(
            subscribe = {
                if (coordinator.state.value.connected) subscribeToEvents()
            },
            initialRefresh = ::refresh,
        )
    }

    /** Publishes an Activity-owned Bluetooth prerequisite failure into screen state. */
    fun reportLocalError(code: String) {
        require(code in LOCAL_PREREQUISITE_ERRORS) { "unsupported_local_error" }
        mutableState.value = mutableState.value.copy(loading = false, errorCode = code)
    }

    suspend fun selectDevice(deviceAddress: String) {
        initialHydration.await()
        if (mutableState.value.activeDeviceAddress.equals(deviceAddress, ignoreCase = true)) return
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        val selected = credentials.selectBondedCompanion(deviceAddress)
            ?: throw TransportException("saved_kitsu_not_found")
        clearLiveData(selected.deviceAddress)
    }

    suspend fun connectDevice(deviceAddress: String) {
        selectDevice(deviceAddress)
        connectAndRefresh(userInitiated = true)
    }

    fun recordUserDisconnectIntent(): Boolean = coordinator.recordUserDisconnectIntent()

    suspend fun disconnectByUser() {
        pairingService.cancelPairing()
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = true)
        mutableState.value = mutableState.value.copy(
            connection = coordinator.state.value,
            loading = false,
            errorCode = null,
            pairing = false,
            pairingProgress = null,
            repairingBluetoothPairing = false,
            bluetoothPairingRepairProgress = null,
        )
    }

    suspend fun refresh() {
        initialHydration.await()
        refreshState(includeMessages = true)
    }

    /**
     * Refreshes the mutable message ring independently of the broader owner snapshot.
     *
     * A valid messages.get response must become visible even when an unrelated status,
     * history, peer, or channel request fails later in the same refresh cycle. Message
     * IDs are stable while their delivery state mutates, so this always replaces the
     * local ring from an after=null firmware snapshot.
     */
    private suspend fun refreshMessages(
        transport: KitsuTransport,
        persist: Boolean,
    ) = messageRefreshMutex.withLock {
        val loaded = MessageSnapshotLoader.loadSnapshot { after, limit ->
            transport.messages(after = after, limit = limit)
        }
        val current = mutableState.value
        val snapshot = current.copy(
            messages = MessageSnapshotPolicy.merge(current.messages, loaded),
            messagesErrorCode = null,
            messageReadErrorCode = if (loaded.items.any { item ->
                    item.journalSession == loaded.journalSession &&
                        item.direction.equals("inbound", ignoreCase = true) &&
                        item.unreadOnKitsu == true
                }
            ) current.messageReadErrorCode else null,
            messageJournalSession = loaded.journalSession,
            messageJournalRevision = loaded.journalRevision,
            messageProtocolVersion = loaded.protocolVersion,
        )
        mutableState.value = snapshot
        if (persist) persistCache(snapshot)
    }

    private suspend fun refreshMessages() {
        coordinator.withTransport { transport ->
            refreshMessages(transport, persist = true)
        }
    }

    suspend fun refreshMessagesOnly() {
        initialHydration.await()
        refreshMessages()
    }

    suspend fun markMessagesRead(expectedSession: String, messageIds: List<String>) {
        try {
            messageReadMutex.withLock {
                val requestedIds = messageIds.distinct()
                if (requestedIds.isEmpty()) return@withLock
                if (requestedIds.size != messageIds.size || requestedIds.size > 24) {
                    throw TransportException("message_read_batch_invalid")
                }
                val before = mutableState.value
                if (before.messageJournalSession != expectedSession) {
                    throw TransportException("message_read_session_changed")
                }
                if (!before.messageMarkReadSupported) {
                    throw TransportException("firmware_operation_unavailable")
                }
                val liveUnreadIds = before.messages.asSequence()
                    .filter { message ->
                        message.journalSession == expectedSession &&
                            message.direction.equals("inbound", ignoreCase = true) &&
                            message.unreadOnKitsu == true
                    }
                    .map(Message::id)
                    .toSet()
                if (!liveUnreadIds.containsAll(requestedIds)) {
                    throw TransportException("message_read_target_stale")
                }

                coordinator.withTransport { transport ->
                    val receipt = try {
                        transport.markMessagesRead(expectedSession, requestedIds)
                    } catch (failure: Throwable) {
                        if (failure is CancellationException) throw failure
                        if ((failure as? TransportException)?.code != "firmware_operation_unavailable") {
                            runCatching { refreshMessages(transport, persist = true) }
                        }
                        throw failure
                    }
                    if (!MessageReadReceiptPolicy.matches(
                            receipt,
                            expectedSession,
                            requestedIds.size,
                        )
                    ) {
                        runCatching { refreshMessages(transport, persist = true) }
                        throw TransportException("message_read_receipt_mismatch")
                    }
                    if (mutableState.value.messageJournalSession != expectedSession) {
                        throw TransportException("message_read_session_changed")
                    }

                    // The authenticated, request-bound mutation receipt is the first
                    // point at which these exact live IDs may clear in the UI.
                    val accepted = mutableState.value.copy(
                        messages = mutableState.value.messages.map { message ->
                            if (message.journalSession == expectedSession && message.id in requestedIds) {
                                message.copy(unreadOnKitsu = false)
                            } else {
                                message
                            }
                        },
                        messageJournalRevision = receipt.journalRevision,
                        messagesErrorCode = null,
                        messageReadErrorCode = null,
                    )
                    mutableState.value = accepted
                    persistCache(accepted)
                    try {
                        refreshMessages(transport, persist = true)
                    } catch (failure: Throwable) {
                        if (failure is CancellationException) throw failure
                        SafeLog.warn("owner_messages", "post_read_refresh_failed", failure)
                        mutableState.value = mutableState.value.copy(
                            messagesErrorCode = failure.transportCodeOr("messages_refresh_failed"),
                        )
                    }
                }
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            reportMessageReadFailure(
                expectedSession,
                failure.transportCodeOr("message_read_failed"),
            )
            throw failure
        }
    }

    fun reportMessageReadFailure(expectedSession: String, code: String) {
        if (mutableState.value.messageJournalSession == expectedSession) {
            mutableState.value = mutableState.value.copy(messageReadErrorCode = code)
        }
    }

    private suspend fun refreshState(includeMessages: Boolean) = refreshMutex.withLock {
        try {
            val nonMessageSnapshot = coordinator.withTransport { transport ->
                val status = try {
                    // The authenticated firmware version selects the exact message operation.
                    // Unknown operations close older sessions, so v2 is never probed.
                    transport.status()
                } catch (statusFailure: Throwable) {
                    if (statusFailure is CancellationException) throw statusFailure
                    if (includeMessages) {
                        // status() did not establish v2 support; the BLE transport
                        // remains on safe legacy v1. Still surface a valid inbox before
                        // propagating the unrelated status failure.
                        try {
                            refreshMessages(transport, persist = false)
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (messageFailure: Throwable) {
                            SafeLog.warn("owner_messages", "messages_refresh_failed", messageFailure)
                            mutableState.value = mutableState.value.copy(
                                messagesErrorCode = messageFailure.transportCodeOr("messages_refresh_failed"),
                            )
                        }
                    }
                    mutableState.value = mutableState.value.copy(messageMarkReadSupported = false)
                    throw statusFailure
                }
                // Capability comes from the authenticated state response and must
                // not wait behind unrelated history/peer/channel endpoints.
                mutableState.value = mutableState.value.copy(
                    messageMarkReadSupported = FirmwareMessageApiPolicy.supportsMarkRead(
                        status.firmwareVersion,
                    ),
                )
                if (includeMessages) {
                    try {
                        // Commit immediately after capability selection. History,
                        // peers, and channels are not prerequisites for showing chat.
                        refreshMessages(transport, persist = false)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        SafeLog.warn("owner_messages", "messages_refresh_failed", failure)
                        mutableState.value = mutableState.value.copy(
                            messagesErrorCode = failure.transportCodeOr("messages_refresh_failed"),
                        )
                    }
                }
                val previous = mutableState.value
                val liveNamespace = OwnerCursorNamespace(ConnectionMode.DIRECT_BLE, status.deviceId)
                val history = transport.history(
                    after = OwnerCursorPolicy.resume(
                        previous.history.lastOrNull()?.cursor,
                        lastLiveNamespace,
                        liveNamespace,
                    ),
                    limit = 100,
                )
                val peers = transport.peers()
                val channels = transport.channels(status.firmwareVersion)
                var encounterCatalogErrorCode: String? = null
                val encounterCatalogSupported = FirmwareEncounterApiPolicy.supportsCatalogV1(
                    status.firmwareVersion,
                )
                val encounterCatalog = if (encounterCatalogSupported) {
                    try {
                        transport.encounterCatalog().items
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        encounterCatalogErrorCode = failure.transportCodeOr("encounter_catalog_refresh_failed")
                        SafeLog.warn("owner_encounter_catalog", encounterCatalogErrorCode!!, failure)
                        emptyList()
                    }
                } else {
                    emptyList()
                }
                var nearbyKitsuErrorCode: String? = null
                val nearbyKitsuSupported = FirmwareEncounterApiPolicy.supportsV1(status.firmwareVersion)
                val nearbyPage = if (nearbyKitsuSupported) {
                    try {
                        transport.nearbyKitsu()
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        nearbyKitsuErrorCode = failure.transportCodeOr("nearby_kitsu_refresh_failed")
                        SafeLog.warn("owner_nearby_kitsu", nearbyKitsuErrorCode!!, failure)
                        null
                    }
                } else {
                    null
                }
                var funErrorCode: String? = null
                val funSupported = FirmwareFunApiPolicy.supportsV1(status.firmwareVersion)
                val funState = if (funSupported) {
                    try {
                        transport.funState()
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        funErrorCode = failure.transportCodeOr("fun_state_refresh_failed")
                        SafeLog.warn("owner_fun", funErrorCode!!, failure)
                        null
                    }
                } else {
                    null
                }
                val replaceHistory = OwnerCursorPolicy.shouldReplace(
                    lastLiveNamespace,
                    liveNamespace,
                    history.cursorExpired,
                )
                lastLiveNamespace = liveNamespace
                OwnerNonMessageSnapshot(
                    connection = coordinator.state.value,
                    status = status,
                    history = if (replaceHistory) history.items else
                        (previous.history + history.items).distinctBy { it.id }.takeLast(CachePolicy.MAX_HISTORY),
                    peers = peers.items,
                    channels = channels,
                    encounterCatalog = encounterCatalog,
                    encounterCatalogSupported = encounterCatalogSupported,
                    encounterCatalogErrorCode = encounterCatalogErrorCode,
                    nearbyKitsu = nearbyPage?.items.orEmpty(),
                    nearbyKitsuSupported = nearbyKitsuSupported,
                    nearbyInteractionKinds = nearbyPage?.supportedActions?.toSet().orEmpty(),
                    nearbyKitsuErrorCode = nearbyKitsuErrorCode,
                    funState = funState,
                    funSupported = funSupported,
                    funErrorCode = funErrorCode,
                    messageMarkReadSupported = FirmwareMessageApiPolicy.supportsMarkRead(
                        status.firmwareVersion,
                    ),
                )
            }
            // A messages-only event refresh may have completed while the slower
            // endpoints above were in flight. Commit onto the current state so that
            // newer message rows, message errors, and unrelated in-flight flags win.
            val committed = OwnerRefreshCommitPolicy.apply(
                current = mutableState.value,
                snapshot = nonMessageSnapshot,
            )
            mutableState.value = committed
            persistCache(committed)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            SafeLog.warn("owner_refresh", "refresh_failed", failure)
            mutableState.value = OwnerRefreshCommitPolicy.applyFailure(
                current = mutableState.value,
                connection = coordinator.state.value,
                errorCode = failure.transportCodeOr("refresh_failed"),
            )
        }
    }

    /** Re-establishes trusted firmware time over the authenticated BLE session, then rereads state. */
    suspend fun synchronizeClock() {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        try {
            coordinator.synchronizeClock()
            refresh()
        } finally {
            mutableState.value = mutableState.value.copy(loading = false)
        }
    }

    suspend fun perform(command: ActionCommand): ActionReceipt {
        val receipt = coordinator.withTransport { transport -> transport.action(command) }
        mutableState.value = mutableState.value.copy(errorCode = null)
        if (command.kind == ActionKind.SEND_MESSAGE) {
            // The accepted queued receipt is the submission terminal. Return it now
            // so the composer clears immediately; journal visibility and delivery
            // remain best-effort/event-driven and can never retroactively turn the
            // accepted action into a false failure.
            scope.launch {
                yield()
                try {
                    refreshMessages()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Throwable) {
                    SafeLog.warn("owner_messages", "post_send_refresh_failed", failure)
                    mutableState.value = mutableState.value.copy(
                        messagesErrorCode = failure.transportCodeOr("messages_refresh_failed"),
                    )
                }
            }
        } else {
            refresh()
        }
        return receipt
    }

    suspend fun advertiseOnce(command: ActionCommand): ActionReceipt {
        if (command.kind != ActionKind.ADVERTISE_ONCE) {
            throw IllegalArgumentException("advertise_command_required")
        }
        mutableState.value = mutableState.value.copy(meshAdvertisementInFlight = true, errorCode = null)
        return try {
            val receipt = awaitAdvertiseReceipt(advertiseSubmissionTimeoutMillis) {
                coordinator.withTransport { it.action(command) }
            }
            val refreshedStatus = try {
                // The receipt proves queue admission; authenticated state supplies the
                // selected scope's volatile lifecycle without making it up on the phone.
                // Firmware retains the two scope records independently.
                coordinator.withTransport { it.status() }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                SafeLog.warn("advertise_status", "advertise_status_refresh_failed", failure)
                null
            }
            mutableState.value = acceptedAdvertiseState(
                current = mutableState.value,
                scope = requireNotNull(command.advertiseScope),
                refreshedStatus = refreshedStatus,
            )
            if (refreshedStatus != null) persistCache(mutableState.value)
            receipt
        } catch (cancelled: CancellationException) {
            // Cancellation is lifecycle control, not an invitation to issue more
            // BLE reads. The finally block below is the only required cleanup.
            throw cancelled
        } catch (failure: Throwable) {
            val code = (failure as? TransportException)?.code ?: "advertise_failed"
            mutableState.value = mutableState.value.withAdvertiseReadiness(
                ready = false,
                retryAfterMs = 0L,
                error = code,
            )
            throw failure
        } finally {
            mutableState.value = mutableState.value.copy(meshAdvertisementInFlight = false)
        }
    }

    suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        mutableState.value = mutableState.value.copy(meshConfigurationInFlight = true, errorCode = null)
        return try {
            coordinator.withTransport { it.configureMesh(enabled) }.also { refresh() }
        } finally {
            mutableState.value = mutableState.value.copy(meshConfigurationInFlight = false)
        }
    }

    /** Reads the bounded, hardware-bound encounter-code journal over authenticated BLE. */
    suspend fun loadEncounterCodes(): List<EncounterUnlockCode> {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        val records = linkedMapOf<String, EncounterUnlockCode>()
        val seenCursors = linkedSetOf<String>()
        var after: String? = null
        var hasMore: Boolean
        do {
            val page = coordinator.withTransport { transport ->
                transport.encounterCodes(after, EncounterCodePolicy.MAX_PAGE_SIZE)
            }
            page.items.forEach { value ->
                if (records.put(value.vaultKey, value) != null) {
                    throw TransportException("malformed_encounter_codes")
                }
            }
            val next = page.cursor
            if (page.hasMore && (next == null || !seenCursors.add(next))) {
                throw TransportException("malformed_encounter_codes")
            }
            after = next
            hasMore = page.hasMore
        } while (hasMore && records.size < EncounterCodePolicy.MAX_VAULT_RECORDS)
        return records.values.take(EncounterCodePolicy.MAX_VAULT_RECORDS)
    }

    /** Remote care is a distinct protocol and never mutates the local companion's care state. */
    suspend fun interactWithNeighbor(
        command: NeighborInteractionCommand,
    ): NeighborInteractionReceipt {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        NeighborInteractionCapabilityPolicy.validationError(
            supported = mutableState.value.nearbyInteractionKinds,
            requested = command.kind,
        )?.let { code ->
            throw TransportException(code)
        }
        return coordinator.withTransport { transport ->
            transport.neighborInteraction(command).also {
                // Firmware consumes this sequence when it accepts the targeted action.
                // Advance only the matching live session; local companion care is untouched.
                mutableState.value = mutableState.value.copy(
                    nearbyKitsu = mutableState.value.nearbyKitsu.map { neighbor ->
                        if (
                            neighbor.deviceId == command.targetDeviceId &&
                            neighbor.sessionNonce == command.targetSessionNonce &&
                            neighbor.nextSequence == command.sequence
                        ) {
                            neighbor.copy(
                                nextSequence = if (command.sequence == 65_535L) 1L else command.sequence + 1L,
                            )
                        } else neighbor
                    },
                    nearbyKitsuErrorCode = null,
                )
            }
        }
    }

    suspend fun startExpedition(duration: ExpeditionDuration): FunState =
        mutateFun { it.startExpedition(duration) }

    suspend fun claimExpedition(): FunState = mutateFun(KitsuTransport::claimExpedition)

    suspend fun startStory(trigger: StoryTrigger): FunState =
        mutateFun { it.startStory(trigger) }

    suspend fun advanceStory(storyId: Int): FunState =
        mutateFun { it.advanceStory(storyId) }

    suspend fun chooseStory(storyId: Int, choice: Int): FunState =
        mutateFun { it.chooseStory(storyId, choice) }

    suspend fun scanParty(): FunState = mutateFun(KitsuTransport::scanParty)

    suspend fun hostParty(): FunState = mutateFun(KitsuTransport::hostParty)

    suspend fun joinParty(command: PartyJoinCommand): FunState =
        mutateFun { it.joinParty(command) }

    suspend fun beginParty(): FunState = mutateFun(KitsuTransport::beginParty)

    suspend fun chooseParty(command: PartyRoundCommand): FunState =
        mutateFun { it.chooseParty(command) }

    suspend fun leaveParty(): FunState = mutateFun(KitsuTransport::leaveParty)

    private suspend fun mutateFun(
        call: suspend (KitsuTransport) -> FunState,
    ): FunState = refreshMutex.withLock {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        if (!mutableState.value.funSupported) throw TransportException("firmware_operation_unavailable")
        if (mutableState.value.funMutationInFlight) throw TransportException("fun_action_in_flight")
        mutableState.value = mutableState.value.copy(
            funMutationInFlight = true,
            funErrorCode = null,
        )
        try {
            coordinator.withTransport(call).also { updated ->
                mutableState.value = mutableState.value.copy(
                    funState = updated,
                    funErrorCode = null,
                )
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            throw failure
        } finally {
            mutableState.value = mutableState.value.copy(funMutationInFlight = false)
        }
    }

    suspend fun pairController(label: String) {
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        mutableState.value = mutableState.value.copy(pairing = true, pairingProgress = null, errorCode = null)
        try {
            pairingService.pairController(label) { progress ->
                mutableState.value = mutableState.value.copy(pairing = true, pairingProgress = progress)
            }
            reloadDevices()
            mutableState.value = mutableState.value.copy(pairing = false, pairingProgress = null)
            connectAndRefresh(userInitiated = true)
        } catch (failure: Throwable) {
            runCatching { reloadDevices() }
            mutableState.value = mutableState.value.copy(
                pairing = false,
                pairingProgress = null,
                errorCode = (failure as? PairingException)?.code ?: "pairing_failed",
            )
            throw failure
        }
    }

    suspend fun finishPendingPairing() {
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        mutableState.value = mutableState.value.copy(
            pairing = true,
            pairingProgress = null,
            errorCode = null,
        )
        try {
            pairingService.finishPendingPairing { progress ->
                mutableState.value = mutableState.value.copy(
                    pairing = true,
                    pairingProgress = progress,
                )
            }
            reloadDevices()
            mutableState.value = mutableState.value.copy(pairing = false, pairingProgress = null)
            connectAndRefresh(userInitiated = true)
        } catch (failure: Throwable) {
            runCatching { reloadDevices() }
            mutableState.value = mutableState.value.copy(
                pairing = false,
                pairingProgress = null,
                errorCode = (failure as? PairingException)?.code ?: "pairing_recovery_failed",
            )
            throw failure
        }
    }

    fun cancelPairing() = pairingService.cancelPairing()

    /**
     * Repairs only the selected saved controller's Android SMP bond.
     *
     * The controller ID/root is snapshotted and verified unchanged. The one GATT
     * connection below is the only retry allowed after a newly completed OS bond.
     */
    suspend fun repairBluetoothPairing(deviceAddress: String) {
        initialHydration.await()
        val before = credentials.bondedCompanions().firstOrNull {
            it.deviceAddress.equals(deviceAddress, ignoreCase = true)
        } ?: throw PairingException("saved_controller_not_found")
        val active = credentials.bondedCompanion()
        if (active == null || !active.deviceAddress.equals(deviceAddress, ignoreCase = true) ||
            active.controllerIdB64 != before.controllerIdB64 ||
            active.controllerRootB64 != before.controllerRootB64
        ) throw PairingException("select_saved_controller_before_repair")

        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        mutableState.value = mutableState.value.copy(
            loading = false,
            errorCode = null,
            repairingBluetoothPairing = true,
            bluetoothPairingRepairProgress = BluetoothPairingRepairProgress(
                BluetoothPairingRepairStage.CHECKING_SAVED_CONTROLLER,
                "checking_saved_controller",
            ),
        )
        try {
            val repaired = pairingService.repairBluetoothPairing(deviceAddress) { progress ->
                mutableState.value = mutableState.value.copy(
                    repairingBluetoothPairing = true,
                    bluetoothPairingRepairProgress = progress,
                )
            }
            val after = credentials.bondedCompanions().firstOrNull {
                it.deviceAddress.equals(deviceAddress, ignoreCase = true)
            }
            if (repaired != before || after != before) {
                throw PairingException("saved_controller_changed_during_repair")
            }

            mutableState.value = mutableState.value.copy(
                bluetoothPairingRepairProgress = BluetoothPairingRepairProgress(
                    BluetoothPairingRepairStage.CONNECTING_GATT,
                    "one_fresh_gatt_retry",
                ),
            )
            val connection = coordinator.connect(userInitiated = true)
            if (!connection.connected) {
                val code = if (ControllerForgetPolicy.controllerAlreadyAbsent(connection.detail)) {
                    BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING
                } else connection.detail
                throw PairingException(code)
            }
            mutableState.value = mutableState.value.copy(
                bluetoothPairingRepairProgress = BluetoothPairingRepairProgress(
                    BluetoothPairingRepairStage.VERIFYING_CONTROLLER,
                    "saved_controller_authenticated",
                ),
            )
            OwnerConnectRefreshOrder.run(
                subscribe = {
                    if (coordinator.state.value.connected) subscribeToEvents()
                },
                initialRefresh = ::refresh,
            )
            mutableState.value = mutableState.value.copy(
                repairingBluetoothPairing = false,
                bluetoothPairingRepairProgress = BluetoothPairingRepairProgress(
                    BluetoothPairingRepairStage.COMPLETE,
                    "bluetooth_pairing_repaired_controller_kept",
                ),
                errorCode = null,
            )
        } catch (failure: Throwable) {
            val code = (failure as? PairingException)?.code ?: "bluetooth_pairing_repair_failed"
            mutableState.value = mutableState.value.copy(
                loading = false,
                repairingBluetoothPairing = false,
                bluetoothPairingRepairProgress = null,
                errorCode = code,
            )
            throw failure
        }
    }

    fun cancelBluetoothPairingRepair() = pairingService.cancelPairing()

    /** Revokes this app's authenticated controller on Kitsu before deleting the local root. */
    suspend fun forgetController(deviceAddress: String) {
        val pending = credentials.pendingControllerForgetAddress()
        if (pending != null && !pending.equals(deviceAddress, ignoreCase = true)) {
            throw TransportException("controller_forget_pending")
        }
        selectDevice(deviceAddress)

        var connection = coordinator.state.value
        if (!connection.connected || !coordinator.isConnectedTo(deviceAddress)) {
            connection = coordinator.connect(userInitiated = true)
        }
        if (!connection.connected) {
            // The owner explicitly chose Forget. An authenticated firmware
            // rejection proves this saved root cannot name a live controller,
            // including after an on-device recovery removed it first.
            if (ControllerForgetPolicy.controllerAlreadyAbsent(connection.detail)) {
                completeControllerForget(deviceAddress)
                return
            }
            throw TransportException(connection.detail)
        }
        credentials.savePendingControllerForgetAddress(deviceAddress)
        reloadDevices()

        try {
            val receipt = coordinator.withTransport { it.forgetController() }
            if (!receipt.accepted) throw TransportException(receipt.error ?: "controller_forget_failed")
            completeControllerForget(deviceAddress)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: TransportException) {
            if (failure.code !in FORGET_OUTCOME_UNKNOWN) throw failure
            coordinator.disconnect(suppressAutomaticReconnect = false)
            val verification = coordinator.connect(userInitiated = true)
            if (ControllerForgetPolicy.controllerAlreadyAbsent(verification.detail)) {
                completeControllerForget(deviceAddress)
            } else {
                throw TransportException("controller_forget_outcome_unknown", failure)
            }
        }
    }

    suspend fun installFirmware(
        packageFile: VerifiedFirmwarePackage,
        onProgress: (FirmwareInstallProgress) -> Unit,
    ) {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        stopEvents()
        var cancelled = false
        try {
        val total = packageFile.manifest.imageBytes
        onProgress(FirmwareInstallProgress(FirmwareInstallStage.PREPARING, packageFile.manifest.firmwareVersion, 0, total))
        var status = coordinator.withTransport { it.firmwareUpdateStatus() }
        if (status.state == "pending_verify") {
            validateUpdateBinding(status, packageFile.updateId, total)
            onProgress(FirmwareInstallProgress(FirmwareInstallStage.REBOOTING, packageFile.manifest.firmwareVersion, total, total))
            delay(31_000L)
            status = coordinator.withTransport { it.firmwareUpdateStatus() }
            validateUpdateBinding(status, packageFile.updateId, total)
            if (status.state == "rolled_back") throw TransportException("firmware_rolled_back")
            if (status.state != "confirmed") throw TransportException("firmware_confirmation_pending")
        }
        if (status.state == "confirmed" && status.updateId == packageFile.updateId) {
            validateUpdateBinding(status, packageFile.updateId, total)
            onProgress(FirmwareInstallProgress(FirmwareInstallStage.COMPLETE, packageFile.manifest.firmwareVersion, total, total))
            refresh()
            return
        }
        if (status.state in setOf("failed", "rolled_back")) {
            if (status.updateId != null && status.updateId != packageFile.updateId) {
                throw TransportException("update_conflict")
            }
            val aborted = coordinator.withTransport {
                it.abortFirmwareUpdate(status.updateId ?: packageFile.updateId)
            }
            requireIdleAfterAbort(aborted)
            status = coordinator.withTransport { it.firmwareUpdateStatus() }
            requireIdleAfterAbort(status)
        }
        if (status.state == "receiving" && status.updateId != packageFile.updateId) {
            throw TransportException("update_conflict")
        }
        val readyToReboot = status.state == "ready_to_reboot"
        var receipt = if (readyToReboot) {
            validateUpdateBinding(status, packageFile.updateId, total)
            status
        } else {
            if (status.state !in setOf("idle", "receiving", "confirmed")) {
                throw TransportException("invalid_state")
            }
            coordinator.withTransport {
                it.beginFirmwareUpdate(packageFile.manifestBytes, packageFile.signature)
            }.also {
                validateUpdateBinding(it, packageFile.updateId, total)
                if (it.state != "receiving") throw TransportException("invalid_state")
            }
        }
        var offset = receipt.nextOffset
        if (offset !in 0..total) throw TransportException("malformed_firmware_update_receipt")
        val chunkBytes = coordinator.withTransport { it.firmwareTransferChunkBytes() }
            .takeIf { it == 256 || it == 4_096 } ?: 256
        if (!readyToReboot) {
            FileInputStream(packageFile.imageFile).use { image ->
                skipExactly(image, offset)
                while (offset < total) {
                    val data = image.readNBytesCompat(minOf(chunkBytes, total - offset))
                    if (data.isEmpty()) throw TransportException("truncated_imported_image")
                    receipt = writeWithRecovery(packageFile, offset, data)
                    validateUpdateBinding(receipt, packageFile.updateId, total)
                    if (receipt.state != "receiving") throw TransportException("invalid_state")
                    if (receipt.nextOffset < offset + data.size) {
                        throw TransportException("firmware_update_progress_mismatch")
                    }
                    if (receipt.nextOffset > offset + data.size) {
                        skipExactly(image, receipt.nextOffset - (offset + data.size))
                    }
                    offset = receipt.nextOffset
                    onProgress(FirmwareInstallProgress(
                        FirmwareInstallStage.TRANSFERRING,
                        packageFile.manifest.firmwareVersion,
                        offset,
                        total,
                    ))
                }
            }
            onProgress(FirmwareInstallProgress(FirmwareInstallStage.VERIFYING, packageFile.manifest.firmwareVersion, total, total))
            receipt = coordinator.withTransport { it.finishFirmwareUpdate(packageFile.updateId) }
            validateUpdateBinding(receipt, packageFile.updateId, total)
            if (receipt.state != "ready_to_reboot" || receipt.nextOffset != total) {
                throw TransportException("firmware_finish_rejected")
            }
        }
        onProgress(FirmwareInstallProgress(FirmwareInstallStage.READY_TO_REBOOT, packageFile.manifest.firmwareVersion, total, total))
        receipt = coordinator.withTransport { it.rebootFirmwareUpdate(packageFile.updateId) }
        validateUpdateBinding(receipt, packageFile.updateId, total)
        if (!receipt.scheduled || receipt.state != "ready_to_reboot") {
            throw TransportException("reboot_not_ready")
        }
        onProgress(FirmwareInstallProgress(FirmwareInstallStage.REBOOTING, packageFile.manifest.firmwareVersion, total, total))
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        var reconnected = false
        repeat(3) { attempt ->
            if (!reconnected) {
                delay(3_000L + attempt * 1_000L)
                reconnected = coordinator.connect(userInitiated = true).connected
            }
        }
        if (!reconnected) throw TransportException("firmware_reconnect_required")
        var bootStatus = coordinator.withTransport { it.firmwareUpdateStatus() }
        validateUpdateBinding(bootStatus, packageFile.updateId, total)
        if (bootStatus.state == "pending_verify") {
            delay(31_000L)
            bootStatus = coordinator.withTransport { it.firmwareUpdateStatus() }
            validateUpdateBinding(bootStatus, packageFile.updateId, total)
        }
        when (bootStatus.state) {
            "confirmed" -> {
                onProgress(FirmwareInstallProgress(
                    FirmwareInstallStage.COMPLETE,
                    packageFile.manifest.firmwareVersion,
                    total,
                    total,
                ))
                refresh()
            }
            "rolled_back" -> throw TransportException("firmware_rolled_back")
            else -> throw TransportException("firmware_confirmation_pending")
        }
        } catch (failure: CancellationException) {
            cancelled = true
            throw failure
        } finally {
            // Cancellation is followed immediately by an authenticated abort in the
            // ViewModel. Do not let live refresh traffic interleave with that cleanup.
            if (!cancelled && coordinator.state.value.connected) subscribeToEvents()
        }
    }

    suspend fun abortFirmwareUpdate(updateId: String) {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        stopEvents()
        try {
            val status = coordinator.withTransport { it.firmwareUpdateStatus() }
            if (status.state == "idle" && status.updateId == null) return
            if (status.updateId != null && status.updateId != updateId) {
                throw TransportException("update_conflict")
            }
            if (status.updateId == null && status.state != "failed") {
                throw TransportException("invalid_update_id")
            }
            val receipt = coordinator.withTransport {
                it.abortFirmwareUpdate(status.updateId ?: updateId)
            }
            requireIdleAfterAbort(receipt, "firmware_abort_failed")
        } finally {
            if (coordinator.state.value.connected) subscribeToEvents()
        }
    }

    suspend fun resetInterruptedFirmwareUpdate() {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        stopEvents()
        try {
            val status = coordinator.withTransport { it.firmwareUpdateStatus() }
            if (status.state == "idle" && status.updateId == null) return
            if (status.state in setOf("pending_verify", "confirmed")) {
                throw TransportException("firmware_reset_not_allowed")
            }
            val updateId = status.updateId ?: if (status.state == "failed") {
                UNBOUND_UPDATE_ABORT_ID
            } else {
                throw TransportException("invalid_update_id")
            }
            val reset = coordinator.withTransport { it.abortFirmwareUpdate(updateId) }
            requireIdleAfterAbort(reset)
        } finally {
            if (coordinator.state.value.connected) subscribeToEvents()
        }
    }

    private suspend fun writeWithRecovery(
        packageFile: VerifiedFirmwarePackage,
        offset: Int,
        data: ByteArray,
    ): FirmwareUpdateReceipt {
        var lastFailure: TransportException? = null
        var recoveryRequired = false
        repeat(3) { attempt ->
            if (recoveryRequired) {
                coordinator.disconnect(suppressAutomaticReconnect = false)
                delay(500L * attempt.coerceAtLeast(1))
                val connection = coordinator.connect(userInitiated = true)
                if (!connection.connected) {
                    lastFailure = TransportException(connection.detail)
                    if (attempt == 2) throw TransportException(connection.detail)
                    return@repeat
                }
                val status = coordinator.withTransport { it.firmwareUpdateStatus() }
                validateUpdateBinding(status, packageFile.updateId, packageFile.manifest.imageBytes)
                if (status.nextOffset >= offset + data.size) return status
                if (status.nextOffset != offset) throw TransportException("firmware_update_progress_mismatch")
                if (status.state == "failed") {
                    throw TransportException(status.error ?: "firmware_update_failed")
                }
                if (status.state != "receiving") throw TransportException("invalid_state")
                // A recovered journal is not writable until the same signed begin is replayed.
                val resumed = coordinator.withTransport {
                    it.beginFirmwareUpdate(packageFile.manifestBytes, packageFile.signature)
                }
                validateUpdateBinding(resumed, packageFile.updateId, packageFile.manifest.imageBytes)
                if (resumed.nextOffset >= offset + data.size) return resumed
                if (resumed.nextOffset != offset || resumed.state != "receiving") {
                    throw TransportException("firmware_update_progress_mismatch")
                }
            }
            try {
                return coordinator.withTransport {
                    it.writeFirmwareUpdate(packageFile.updateId, offset, data)
                }
            } catch (failure: TransportException) {
                lastFailure = failure
                if (failure.code !in TRANSIENT_UPDATE_FAILURES || attempt == 2) throw failure
                recoveryRequired = true
            }
        }
        throw lastFailure ?: TransportException("firmware_update_failed")
    }

    private fun validateUpdateBinding(receipt: FirmwareUpdateReceipt, updateId: String, total: Int) {
        if (receipt.updateId != updateId || receipt.imageBytes != total || receipt.nextOffset !in 0..total) {
            throw TransportException("firmware_update_binding_failed")
        }
    }

    private fun requireIdleAfterAbort(
        receipt: FirmwareUpdateReceipt,
        failureCode: String = "firmware_update_reset_failed",
    ) {
        if (receipt.state != "idle" || receipt.updateId != null ||
            receipt.imageBytes != 0 || receipt.nextOffset != 0
        ) throw TransportException(failureCode)
    }

    private suspend fun completeControllerForget(deviceAddress: String) {
        stopEvents()
        coordinator.disconnect(suppressAutomaticReconnect = false)
        credentials.removeBondedCompanion(deviceAddress)
        credentials.savePendingControllerForgetAddress(null)
        withContext(Dispatchers.IO) { cache.clear() }
        lastLiveNamespace = null
        reloadDevices()
        mutableState.value = mutableState.value.copy(
            status = null,
            history = emptyList(),
            peers = emptyList(),
            encounterCatalog = emptyList(),
            encounterCatalogSupported = false,
            encounterCatalogErrorCode = null,
            nearbyKitsu = emptyList(),
            nearbyKitsuSupported = false,
            nearbyInteractionKinds = emptySet(),
            nearbyKitsuErrorCode = null,
            funState = null,
            funSupported = false,
            funErrorCode = null,
            funMutationInFlight = false,
            messages = emptyList(),
            messagesErrorCode = null,
            messageJournalSession = null,
            messageJournalRevision = null,
            messageProtocolVersion = 1,
            messageMarkReadSupported = false,
            messageReadErrorCode = null,
            channels = emptyList(),
            errorCode = null,
        )
    }

    private suspend fun reloadDevices() {
        val devices = credentials.bondedCompanions()
        val active = credentials.bondedCompanion()
        mutableState.value = mutableState.value.copy(
            savedKitsu = devices,
            activeDeviceAddress = active?.deviceAddress,
            pendingPairing = reconcilePendingPairing(devices),
            pendingForgetAddress = reconcilePendingForget(devices),
        )
    }

    private suspend fun reconcilePendingPairing(devices: List<BondedCompanion>): BondedCompanion? {
        val pending = credentials.pendingBondedCompanion() ?: return null
        val alreadyPromoted = devices.any {
            it.deviceAddress.equals(pending.deviceAddress, ignoreCase = true) &&
                it.controllerIdB64 == pending.controllerIdB64 &&
                it.controllerRootB64 == pending.controllerRootB64
        }
        if (!alreadyPromoted) return pending
        credentials.savePendingBondedCompanion(null)
        return null
    }

    private suspend fun reconcilePendingForget(devices: List<BondedCompanion>): String? {
        val pending = credentials.pendingControllerForgetAddress() ?: return null
        if (devices.any { it.deviceAddress.equals(pending, ignoreCase = true) }) return pending
        // The accepted receipt was already acted on and the saved root was removed,
        // but a process stop may have interrupted the following marker cleanup.
        credentials.savePendingControllerForgetAddress(null)
        return null
    }

    private suspend fun clearLiveData(activeAddress: String) {
        withContext(Dispatchers.IO) { cache.clear() }
        lastLiveNamespace = null
        reloadDevices()
        mutableState.value = mutableState.value.copy(
            activeDeviceAddress = activeAddress,
            status = null,
            history = emptyList(),
            peers = emptyList(),
            encounterCatalog = emptyList(),
            encounterCatalogSupported = false,
            encounterCatalogErrorCode = null,
            nearbyKitsu = emptyList(),
            nearbyKitsuSupported = false,
            nearbyInteractionKinds = emptySet(),
            nearbyKitsuErrorCode = null,
            funState = null,
            funSupported = false,
            funErrorCode = null,
            funMutationInFlight = false,
            messages = emptyList(),
            messagesErrorCode = null,
            messageJournalSession = null,
            messageJournalRevision = null,
            messageProtocolVersion = 1,
            messageMarkReadSupported = false,
            messageReadErrorCode = null,
            channels = emptyList(),
            connection = coordinator.state.value,
            errorCode = null,
        )
    }

    private suspend fun stopEvents() {
        eventJob?.cancelAndJoin()
        eventJob = null
    }

    private suspend fun subscribeToEvents() {
        stopEvents()
        val cursor = mutableState.value.history.lastOrNull()?.cursor ?: mutableState.value.status?.cursor
        eventJob = scope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                coordinator.withTransport { transport ->
                    transport.events(cursor).conflate().collect {
                        // companion.refresh has no event subtype. Publish the full
                        // mutable chat ring first so inbound and delivery transitions
                        // are not held hostage by slower, unrelated reads.
                        try {
                            refreshMessages(transport, persist = false)
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Throwable) {
                            SafeLog.warn("owner_messages", "event_messages_refresh_failed", failure)
                            mutableState.value = mutableState.value.copy(
                                messagesErrorCode = failure.transportCodeOr("messages_refresh_failed"),
                            )
                        }
                        refreshState(includeMessages = false)
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                SafeLog.warn("owner_events", "event_stream_ended", failure)
            }
        }
    }

    private suspend fun persistCache(snapshot: OwnerState) = withContext(Dispatchers.IO) {
        runCatching {
            cache.write(
                CacheSnapshot(
                    status = snapshot.status,
                    history = snapshot.history,
                    peers = snapshot.peers,
                    channels = snapshot.channels,
                    messages = snapshot.messages,
                    historyCursor = snapshot.history.lastOrNull()?.cursor,
                    messageCursor = snapshot.messages.lastOrNull()?.cursor,
                    writtenAt = System.currentTimeMillis() / 1_000L,
                ),
            )
        }.onFailure { SafeLog.warn("owner_cache", "cache_write_failed", it) }
    }

    private fun Throwable.transportCodeOr(fallback: String): String =
        (this as? TransportException)?.code ?: fallback

    private fun skipExactly(input: FileInputStream, bytes: Int) {
        var remaining = bytes.toLong()
        while (remaining > 0) {
            val skipped = input.skip(remaining)
            if (skipped <= 0) throw TransportException("truncated_imported_image")
            remaining -= skipped
        }
    }

    private fun FileInputStream.readNBytesCompat(bytes: Int): ByteArray {
        val output = ByteArray(bytes)
        var offset = 0
        while (offset < bytes) {
            val count = read(output, offset, bytes - offset)
            if (count < 0) break
            offset += count
        }
        return if (offset == bytes) output else output.copyOf(offset)
    }

    private fun OwnerState.withAdvertiseReadiness(
        ready: Boolean,
        retryAfterMs: Long,
        error: String?,
    ): OwnerState {
        val currentStatus = status ?: return copy(errorCode = error)
        return copy(
            status = currentStatus.copy(
                mesh = currentStatus.mesh.copy(
                    advertiseReady = ready,
                    advertiseRetryAfterMs = retryAfterMs,
                    advertiseError = error,
                ),
            ),
            errorCode = null,
        )
    }

    private companion object {
        val LOCAL_PREREQUISITE_ERRORS = setOf(
            "bluetooth_permission_required",
            "pairing_bluetooth_permission_required",
            BluetoothPairingRepairPolicy.PERMISSION_REQUIRED,
        )
        const val UNBOUND_UPDATE_ABORT_ID =
            "0000000000000000000000000000000000000000000000000000000000000000"
        val FORGET_OUTCOME_UNKNOWN = setOf("request_timeout", "disconnected", "gatt_disconnected")
        val TRANSIENT_UPDATE_FAILURES = setOf(
            "request_timeout", "disconnected", "gatt_disconnected", "gatt_write_failed",
        )
    }
}

internal object NeighborInteractionCapabilityPolicy {
    fun validationError(
        supported: Set<NeighborInteractionKind>,
        requested: NeighborInteractionKind,
    ): String? = if (requested in supported) null else "neighbor_action_unsupported"
}

/**
 * Applies an accepted advertise receipt from authenticated state. Firmware keeps
 * zero-hop Nearby and Mesh-wide flood lifecycle records independent.
 */
internal fun acceptedAdvertiseState(
    current: OwnerState,
    scope: AdvertiseScope,
    refreshedStatus: KitsuStatus?,
): OwnerState {
    if (refreshedStatus != null) {
        return current.copy(
            status = refreshedStatus,
            messageMarkReadSupported = FirmwareMessageApiPolicy.supportsMarkRead(
                refreshedStatus.firmwareVersion,
            ),
            errorCode = null,
        )
    }
    val currentStatus = current.status ?: return current.copy(errorCode = null)
    return current.copy(
        status = currentStatus.copy(
            mesh = currentStatus.mesh.copy(
                advertiseReady = false,
                advertiseRetryAfterMs = ADVERTISE_COOLDOWN_MILLIS,
                advertiseError = ADVERTISE_COOLDOWN,
            ),
        ),
        errorCode = null,
    )
}
