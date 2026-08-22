package app.kitsu.mobile.repository

import app.kitsu.mobile.cache.CacheSnapshot
import app.kitsu.mobile.cache.CachePolicy
import app.kitsu.mobile.cache.EncryptedBoundedCache
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.connection.ConnectionState
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiRetryReceipt
import app.kitsu.mobile.pairing.ControllerPairingProgress
import app.kitsu.mobile.pairing.ControllerPairingService
import app.kitsu.mobile.pairing.PairingException
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.GatewayCatalogService
import app.kitsu.mobile.transport.GatewayProvisioningRecord
import app.kitsu.mobile.transport.RemoteCompanion
import app.kitsu.mobile.transport.RemoteCompanionCatalog
import app.kitsu.mobile.transport.OwnerEnrollmentService
import app.kitsu.mobile.transport.ProvisioningPolicy
import app.kitsu.mobile.transport.TransportException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

data class OwnerState(
    val connection: ConnectionState = ConnectionState(),
    val status: KitsuStatus? = null,
    val history: List<HistoryEntry> = emptyList(),
    val peers: List<Peer> = emptyList(),
    val messages: List<Message> = emptyList(),
    val channels: List<MeshChannel> = emptyList(),
    val loading: Boolean = false,
    val errorCode: String? = null,
    val lastAction: ActionReceipt? = null,
    val remoteCompanions: List<RemoteCompanion> = emptyList(),
    val selectedRemoteCompanionId: String? = null,
    val gatewayProvisioningRecords: List<GatewayProvisioningRecord> = emptyList(),
    val gatewayCatalogLoading: Boolean = false,
    val gatewayCatalogError: String? = null,
    /** Exact non-secret gateway UUID stored on the current direct device in this process. */
    val provisionedGatewayId: String? = null,
    val pairing: Boolean = false,
    val pairingProgress: ControllerPairingProgress? = null,
    val meshConfigurationInFlight: Boolean = false,
)

enum class GatewayEnrollmentStage {
    CREATING_CLAIM,
    WAITING_FOR_PHYSICAL_CONFIRMATION,
    SWITCHING_TO_WIFI,
    POLLING_BACKEND,
    COMPLETE,
}

data class GatewayEnrollmentProgress(
    val stage: GatewayEnrollmentStage,
    val remainingMilliseconds: Int? = null,
)

class OwnerRepository(
    private val coordinator: ConnectionCoordinator,
    private val cache: EncryptedBoundedCache,
    private val remoteCatalog: RemoteCompanionCatalog,
    private val gatewayCatalog: GatewayCatalogService,
    private val pairingService: ControllerPairingService,
    private val enrollmentService: OwnerEnrollmentService,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate),
) {
    private val mutableState = MutableStateFlow(OwnerState())
    val state: StateFlow<OwnerState> = mutableState.asStateFlow()
    private var eventJob: Job? = null
    // A backend cursor is not meaningful to the Heltec (and vice versa), and
    // different companions may use unrelated cursor spaces. Disk cache is
    // display-only until the first successful live refresh establishes this
    // namespace.
    private var lastLiveNamespace: OwnerCursorNamespace? = null
    // The firmware intentionally does not echo trust material or gateway UUID.
    // Retain only the non-secret UUID bound to the exact device on which this
    // process successfully stored the v2 gateway record. Process loss requires
    // the owner to select/store the gateway again before enrollment.
    private var provisionedGatewayBinding: Pair<String, String>? = null

    init {
        scope.launch {
            val cached = withContext(Dispatchers.IO) { cache.read() }
            if (cached != null) {
                mutableState.value = mutableState.value.copy(
                    status = cached.status,
                    history = cached.history,
                    peers = cached.peers,
                    messages = cached.messages,
                )
            }
        }
        scope.launch {
            coordinator.state.collect { connection ->
                mutableState.value = mutableState.value.copy(connection = connection)
            }
        }
    }

    suspend fun connectAndRefresh(userInitiated: Boolean = false) {
        if (mutableState.value.pairing) return
        eventJob?.cancelAndJoin()
        eventJob = null
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        val connection = coordinator.connect(userInitiated)
        if (!connection.connected) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = connection.detail)
            if (connection.detail == "companion_selection_required") refreshRemoteCompanions()
            return
        }
        refresh()
        val refreshedConnection = coordinator.state.value
        if (!refreshedConnection.connected) return
        if (refreshedConnection.mode == ConnectionMode.REMOTE_BACKEND) refreshRemoteCompanions()
        subscribeToEvents()
    }

    /** Explicit owner choice to use the authenticated remote service without a
     * preceding BLE scan. This is intentionally separate from automatic
     * fallback, which still requires the bonded device to be absent. */
    suspend fun connectRemoteAndRefresh() {
        if (mutableState.value.pairing) return
        eventJob?.cancelAndJoin()
        eventJob = null
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        val connection = coordinator.connectRemote(userInitiated = true)
        if (!connection.connected) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = connection.detail)
            if (connection.detail == "companion_selection_required") refreshRemoteCompanions()
            return
        }
        refresh()
        val refreshedConnection = coordinator.state.value
        if (!refreshedConnection.connected || refreshedConnection.mode != ConnectionMode.REMOTE_BACKEND) return
        refreshRemoteCompanions()
        subscribeToEvents()
    }

    suspend fun connectDirectAndRefresh() {
        if (mutableState.value.pairing) return
        eventJob?.cancelAndJoin()
        eventJob = null
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        val connection = coordinator.connectDirect(userInitiated = true)
        if (!connection.connected) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = connection.detail)
            return
        }
        refresh()
        val refreshedConnection = coordinator.state.value
        if (!refreshedConnection.connected || refreshedConnection.mode != ConnectionMode.DIRECT_BLE) return
        subscribeToEvents()
    }

    fun recordUserDisconnectIntent(): Boolean = coordinator.recordUserDisconnectIntent()

    suspend fun disconnectByUser() {
        pairingService.cancelPairing()
        eventJob?.cancelAndJoin()
        eventJob = null
        coordinator.disconnect(suppressAutomaticReconnect = true)
        mutableState.value = mutableState.value.copy(
            connection = coordinator.state.value,
            loading = false,
            errorCode = null,
            lastAction = null,
            pairing = false,
            pairingProgress = null,
        )
    }

    /** Hands an existing nearby GATT session to the opt-in foreground relay. */
    suspend fun releaseDirectForMobileRelay(deviceAddress: String? = null) {
        if (!coordinator.isDirect()) return
        eventJob?.cancelAndJoin()
        eventJob = null
        coordinator.handoffDirectForPublicGateway(deviceAddress)
        mutableState.value = mutableState.value.copy(
            connection = coordinator.state.value,
            loading = false,
        )
    }

    /** Disconnects authenticated remote work without changing an earlier user
     * Disconnect suppression decision. */
    suspend fun handleSignedOut() {
        pairingService.cancelPairing()
        eventJob?.cancelAndJoin()
        eventJob = null
        coordinator.disconnect(suppressAutomaticReconnect = false)
        clearDownloadedOwnerData()
        mutableState.value = mutableState.value.copy(
            connection = coordinator.state.value,
            loading = false,
            errorCode = "signed_out",
            pairing = false,
            pairingProgress = null,
        )
    }

    suspend fun refresh() {
        try {
            val snapshot = coordinator.withTransport { transport ->
                val previous = mutableState.value
                val status = transport.status()
                val liveNamespace = OwnerCursorNamespace(
                    coordinator.state.value.mode,
                    status.deviceId,
                )
                val history = transport.history(
                    after = OwnerCursorPolicy.resume(
                        previous.history.lastOrNull()?.cursor,
                        lastLiveNamespace,
                        liveNamespace,
                    ),
                    limit = 100,
                )
                val peers = transport.peers()
                val messages = transport.messages(
                    after = OwnerCursorPolicy.resume(
                        previous.messages.lastOrNull()?.cursor,
                        lastLiveNamespace,
                        liveNamespace,
                    ),
                    limit = 100,
                )
                val channels = transport.channels()
                val mergedHistory = if (OwnerCursorPolicy.shouldReplace(
                        lastLiveNamespace, liveNamespace, history.cursorExpired,
                    )
                ) history.items else
                    (previous.history + history.items).distinctBy { it.id }.takeLast(CachePolicy.MAX_HISTORY)
                val mergedMessages = if (OwnerCursorPolicy.shouldReplace(
                        lastLiveNamespace, liveNamespace, messages.cursorExpired,
                    )
                ) messages.items else
                    (previous.messages + messages.items).distinctBy { it.id }.takeLast(CachePolicy.MAX_MESSAGES)
                lastLiveNamespace = liveNamespace
                OwnerState(
                    connection = coordinator.state.value,
                    status = status,
                    history = mergedHistory,
                    peers = peers.items,
                    messages = mergedMessages,
                    channels = channels,
                    loading = false,
                    remoteCompanions = previous.remoteCompanions,
                    selectedRemoteCompanionId = remoteCatalog.selectedCompanionId(),
                    gatewayProvisioningRecords = previous.gatewayProvisioningRecords,
                    gatewayCatalogLoading = previous.gatewayCatalogLoading,
                    gatewayCatalogError = previous.gatewayCatalogError,
                    provisionedGatewayId = previous.provisionedGatewayId,
                )
            }
            mutableState.value = snapshot
            persistCache(snapshot)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            SafeLog.warn("owner_refresh", "refresh_failed", failure)
            val remote = coordinator.state.value.mode == ConnectionMode.REMOTE_BACKEND
            if (remote) coordinator.disconnect(suppressAutomaticReconnect = false)
            val code = (failure as? TransportException)?.code ?: "refresh_failed"
            mutableState.value = mutableState.value.copy(
                connection = coordinator.state.value,
                loading = false,
                errorCode = code,
            )
        }
    }

    suspend fun perform(command: ActionCommand): ActionReceipt =
        coordinator.withTransport { transport ->
            transport.action(command).also { receipt ->
                mutableState.value = mutableState.value.copy(lastAction = receipt, errorCode = null)
                refresh()
            }
        }

    suspend fun provisionWifi(credentials: WifiProvisioning): ProvisioningReceipt {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        val (receipt, verifiedStatus) = coordinator.withTransport { transport ->
            val accepted = transport.provisionWifi(credentials)
            if (!accepted.accepted || accepted.state != "stored" || accepted.errorCode != null) {
                throw TransportException(accepted.errorCode ?: "wifi_configuration_rejected")
            }
            val status = try {
                transport.status()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                // The signed write receipt was accepted. Preserve that distinction
                // instead of falsely claiming either verified storage or rejection.
                throw TransportException("wifi_storage_verification_unavailable", failure)
            }
            ProvisioningPolicy.wifiVerificationError(accepted, status)?.let { code ->
                throw TransportException(code)
            }
            accepted to status
        }
        val verifiedSnapshot = mutableState.value.copy(
            status = verifiedStatus,
            connection = coordinator.state.value,
            errorCode = null,
        )
        mutableState.value = verifiedSnapshot
        persistCache(verifiedSnapshot)
        return receipt
    }

    suspend fun retryWifi(): WifiRetryReceipt {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        val receipt = coordinator.withTransport { it.retryWifi() }
        if (!receipt.accepted || receipt.state != "retrying" || receipt.errorCode != null) {
            throw TransportException(receipt.errorCode ?: "wifi_retry_rejected")
        }
        repeat(WIFI_RETRY_STATUS_POLL_COUNT) { attempt ->
            if (attempt > 0) delay(WIFI_RETRY_STATUS_POLL_INTERVAL_MS)
            val status = try {
                coordinator.withTransport { it.status() }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                // The authenticated retry receipt is still valid even if a
                // later state poll is interrupted.
                SafeLog.warn("wifi_retry", "status_poll_failed", failure)
                return receipt
            }
            mutableState.value = mutableState.value.copy(
                status = status,
                connection = coordinator.state.value,
                errorCode = null,
            )
            val wifiSettled = status.lan.wifiState !in setOf("connecting", "grace")
            if (attempt > 0 && wifiSettled) {
                persistCache(mutableState.value)
                return receipt
            }
        }
        persistCache(mutableState.value)
        return receipt
    }

    suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt {
        if (!coordinator.isDirect()) throw IllegalStateException("direct_ble_required")
        mutableState.value = mutableState.value.copy(meshConfigurationInFlight = true, errorCode = null)
        return try {
            coordinator.withTransport { it.configureMesh(enabled) }.also { refresh() }
        } finally {
            mutableState.value = mutableState.value.copy(meshConfigurationInFlight = false)
        }
    }

    suspend fun configureGateway(configuration: GatewayConfiguration): GatewayConfigurationReceipt {
        if (!coordinator.isDirect()) throw IllegalStateException("direct_ble_required")
        val deviceId = mutableState.value.status?.deviceId
            ?: coordinator.withTransport { it.status() }.deviceId
        return coordinator.withTransport { it.configureGateway(configuration) }.also { receipt ->
            if (!receipt.accepted || receipt.state != "stored") {
                throw TransportException(receipt.errorCode ?: "gateway_configuration_failed")
            }
            provisionedGatewayBinding = deviceId to configuration.gatewayId
            mutableState.value = mutableState.value.copy(provisionedGatewayId = configuration.gatewayId)
            refresh()
        }
    }

    suspend fun enrollGateway(
        displayName: String,
        onProgress: (GatewayEnrollmentProgress) -> Unit,
    ): GatewayEnrollmentReceipt {
        if (!coordinator.isDirect()) throw TransportException("direct_ble_required")
        val status = mutableState.value.status ?: coordinator.withTransport { it.status() }
        if (status.lan.remoteConnectivityAllowed != true) {
            throw TransportException("remote_connectivity_not_allowed")
        }
        if (status.lan.wifiConfigured != true) throw TransportException("wifi_not_configured")
        if (status.lan.gatewayConfigured != true) throw TransportException("not_configured")
        if (status.lan.gatewayEnrolled == true) throw TransportException("already_enrolled")
        val expectedGatewayId = provisionedGatewayBinding
            ?.takeIf { (deviceId, _) -> deviceId == status.deviceId }
            ?.second
            ?: throw TransportException("gateway_identity_unknown")

        onProgress(GatewayEnrollmentProgress(GatewayEnrollmentStage.CREATING_CLAIM))
        val (enrollmentId, begin) = createAndBeginGatewayEnrollment(status.deviceId, displayName.trim())
        val physicalWindow = begin.expiresInMs ?: throw TransportException("malformed_enrollment_receipt")
        onProgress(
            GatewayEnrollmentProgress(
                GatewayEnrollmentStage.WAITING_FOR_PHYSICAL_CONFIRMATION,
                physicalWindow,
            ),
        )

        val deadline = System.nanoTime() + physicalWindow * 1_000_000L
        var receipt = begin
        while (System.nanoTime() < deadline) {
            delay(1_000)
            receipt = coordinator.withTransport { transport ->
                transport.finishGatewayEnrollment(GatewayEnrollmentFinishBody(enrollmentId = enrollmentId))
            }
            if (receipt.accepted && receipt.state == "ready_for_wifi") break
            val remaining = ((deadline - System.nanoTime()) / 1_000_000L).coerceAtLeast(0L).toInt()
            onProgress(
                GatewayEnrollmentProgress(
                    GatewayEnrollmentStage.WAITING_FOR_PHYSICAL_CONFIRMATION,
                    remaining,
                ),
            )
        }
        if (!receipt.accepted || receipt.state != "ready_for_wifi") {
            throw TransportException("expired")
        }

        onProgress(GatewayEnrollmentProgress(GatewayEnrollmentStage.SWITCHING_TO_WIFI))
        eventJob?.cancelAndJoin()
        eventJob = null
        coordinator.beginEnrollmentRemoteHandoff()
        mutableState.value = mutableState.value.copy(connection = coordinator.state.value, loading = false)
        delay(3_000)

        onProgress(GatewayEnrollmentProgress(GatewayEnrollmentStage.POLLING_BACKEND))
        val backendDeadline = System.nanoTime() + 90_000_000_000L
        var connection = coordinator.state.value
        while (!connection.connected && System.nanoTime() < backendDeadline) {
            try {
                val matching = remoteCatalog.companions().filter { it.hardwareUid == status.deviceId }
                if (matching.size > 1) throw TransportException("duplicate_remote_companion")
                if (matching.size == 1) {
                    remoteCatalog.selectCompanion(matching.single().id)
                    connection = coordinator.pollEnrollmentBackend(
                        expectedDeviceId = status.deviceId,
                        expectedGatewayId = expectedGatewayId,
                    )
                    if (connection.detail in REMOTE_BINDING_FAILURES) {
                        throw TransportException(connection.detail)
                    }
                } else {
                    connection = ConnectionState(
                        mode = ConnectionMode.OFFLINE,
                        connected = false,
                        detail = "no_remote_companion",
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: TransportException) {
                if (failure.code in FATAL_ENROLLMENT_BACKEND_FAILURES) throw failure
                connection = ConnectionState(
                    mode = ConnectionMode.OFFLINE,
                    connected = false,
                    detail = failure.code,
                )
                SafeLog.warn("gateway_enrollment", "backend_poll_retry", failure)
            }
            if (!connection.connected) delay(2_000)
        }
        mutableState.value = mutableState.value.copy(connection = connection, loading = false)
        if (!connection.connected || connection.mode != ConnectionMode.REMOTE_BACKEND) {
            throw TransportException(connection.detail.ifBlank { "enrollment_remote_path_not_confirmed" })
        }
        val verifiedRemote = coordinator.withTransport { it.status() }
        app.kitsu.mobile.transport.RemoteSnapshotPolicy.validationError(
            verifiedRemote,
            expectedDeviceId = status.deviceId,
            expectedGatewayId = expectedGatewayId,
        )?.let { throw TransportException(it) }
        refresh()
        val refreshedConnection = coordinator.state.value
        val refreshedStatus = mutableState.value.status
        if (!refreshedConnection.connected || refreshedConnection.mode != ConnectionMode.REMOTE_BACKEND ||
            refreshedStatus == null
        ) {
            throw TransportException("enrollment_remote_path_lost")
        }
        app.kitsu.mobile.transport.RemoteSnapshotPolicy.validationError(
            refreshedStatus,
            expectedDeviceId = status.deviceId,
            expectedGatewayId = expectedGatewayId,
        )?.let { throw TransportException(it) }
        subscribeToEvents()
        onProgress(GatewayEnrollmentProgress(GatewayEnrollmentStage.COMPLETE))
        return receipt
    }

    /**
     * Keeps the one-use claim token in this short-lived stack frame only. The
     * caller receives the public enrollment ID and bound device receipt, never
     * the token-bearing challenge.
     */
    private suspend fun createAndBeginGatewayEnrollment(
        hardwareUid: String,
        displayName: String,
    ): Pair<String, GatewayEnrollmentReceipt> {
        val challenge = enrollmentService.createEnrollment(hardwareUid, displayName)
        val enrollmentId = challenge.enrollment.id
        val receipt = coordinator.withTransport { transport ->
            transport.beginGatewayEnrollment(
                GatewayEnrollmentBeginBody(
                    enrollmentId = enrollmentId,
                    claimToken = challenge.claimToken,
                ),
            )
        }
        return enrollmentId to receipt
    }

    suspend fun pairController(label: String) {
        eventJob?.cancelAndJoin()
        eventJob = null
        coordinator.disconnect()
        provisionedGatewayBinding = null
        mutableState.value = mutableState.value.copy(provisionedGatewayId = null)
        mutableState.value = mutableState.value.copy(
            pairing = true,
            pairingProgress = null,
            errorCode = null,
        )
        try {
            pairingService.pairController(label) { progress ->
                mutableState.value = mutableState.value.copy(
                    pairing = progress.stage != app.kitsu.mobile.pairing.ControllerPairingStage.CANCELLED,
                    pairingProgress = progress,
                )
            }
            mutableState.value = mutableState.value.copy(pairing = false, errorCode = null)
            connectAndRefresh()
        } catch (failure: Throwable) {
            val code = (failure as? PairingException)?.code ?: "pairing_failed"
            mutableState.value = mutableState.value.copy(
                pairing = false,
                pairingProgress = null,
                errorCode = code,
            )
            throw failure
        }
    }

    fun cancelPairing() {
        pairingService.cancelPairing()
    }

    suspend fun refreshRemoteCompanions() {
        runCatching { remoteCatalog.companions() }
            .onSuccess { companions ->
                mutableState.value = mutableState.value.copy(
                    remoteCompanions = companions,
                    selectedRemoteCompanionId = remoteCatalog.selectedCompanionId(),
                )
            }
            .onFailure { failure -> SafeLog.warn("remote_companions", "catalog_failed", failure) }
    }

    suspend fun refreshGatewayCatalog() {
        if (mutableState.value.gatewayCatalogLoading) return
        mutableState.value = mutableState.value.copy(
            gatewayCatalogLoading = true,
            gatewayCatalogError = null,
        )
        try {
            val records = gatewayCatalog.gateways()
            mutableState.value = mutableState.value.copy(
                gatewayProvisioningRecords = records,
                gatewayCatalogLoading = false,
                gatewayCatalogError = if (records.isEmpty()) "no_gateways" else null,
            )
        } catch (cancelled: CancellationException) {
            mutableState.value = mutableState.value.copy(gatewayCatalogLoading = false)
            throw cancelled
        } catch (failure: Throwable) {
            val code = (failure as? TransportException)?.code ?: "gateway_catalog_unavailable"
            SafeLog.warn("gateway_catalog", code, failure)
            mutableState.value = mutableState.value.copy(
                gatewayProvisioningRecords = emptyList(),
                gatewayCatalogLoading = false,
                gatewayCatalogError = code,
            )
        }
    }

    suspend fun selectRemoteCompanion(id: String) {
        val changed = remoteCatalog.selectedCompanionId() != id
        remoteCatalog.selectCompanion(id)
        if (changed) {
            withContext(Dispatchers.IO) { cache.clear() }
            lastLiveNamespace = null
            mutableState.value = mutableState.value.copy(
                status = null,
                history = emptyList(),
                peers = emptyList(),
                messages = emptyList(),
                channels = emptyList(),
                selectedRemoteCompanionId = id,
            )
        }
        connectAndRefresh(userInitiated = false)
    }

    suspend fun clearDownloadedOwnerData() {
        eventJob?.cancelAndJoin()
        withContext(Dispatchers.IO) { cache.clear() }
        lastLiveNamespace = null
        provisionedGatewayBinding = null
        mutableState.value = mutableState.value.copy(
            status = null,
            history = emptyList(),
            peers = emptyList(),
            messages = emptyList(),
            channels = emptyList(),
            lastAction = null,
            remoteCompanions = emptyList(),
            gatewayProvisioningRecords = emptyList(),
            gatewayCatalogLoading = false,
            gatewayCatalogError = null,
            provisionedGatewayId = null,
        )
    }

    private suspend fun subscribeToEvents() {
        eventJob?.cancelAndJoin()
        val cursor = mutableState.value.history.lastOrNull()?.cursor
            ?: mutableState.value.status?.cursor
        eventJob = scope.launch {
            runCatching {
                coordinator.withTransport { transport ->
                    transport.events(cursor).collect { refresh() }
                }
            }.onFailure { failure ->
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
                    messages = snapshot.messages,
                    historyCursor = snapshot.history.lastOrNull()?.cursor,
                    messageCursor = snapshot.messages.lastOrNull()?.cursor,
                    writtenAt = System.currentTimeMillis() / 1000L,
                ),
            )
        }.onFailure { SafeLog.warn("owner_cache", "cache_write_failed", it) }
    }

    private companion object {
        const val WIFI_RETRY_STATUS_POLL_COUNT = 6
        const val WIFI_RETRY_STATUS_POLL_INTERVAL_MS = 750L
        val REMOTE_BINDING_FAILURES = setOf(
            "remote_companion_binding_failed",
            "remote_gateway_binding_failed",
            "remote_provenance_unverified",
            "remote_gateway_unverified",
            "remote_last_seen_invalid",
        )
        val FATAL_ENROLLMENT_BACKEND_FAILURES = REMOTE_BINDING_FAILURES + setOf(
            "duplicate_remote_companion",
            "invalid_companion_id",
            "malformed_response",
            "sign_in_required",
        )
    }
}
