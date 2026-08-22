package app.kitsu.mobile.repository

import app.kitsu.mobile.cache.CachePolicy
import app.kitsu.mobile.cache.CacheSnapshot
import app.kitsu.mobile.cache.EncryptedBoundedCache
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.connection.ConnectionState
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.Peer
import app.kitsu.mobile.pairing.ControllerPairingProgress
import app.kitsu.mobile.pairing.ControllerPairingService
import app.kitsu.mobile.pairing.PairingException
import app.kitsu.mobile.security.BondedCompanion
import app.kitsu.mobile.security.CredentialStore
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.TransportException
import app.kitsu.mobile.update.FirmwareInstallProgress
import app.kitsu.mobile.update.FirmwareInstallStage
import app.kitsu.mobile.update.FirmwareUpdateReceipt
import app.kitsu.mobile.update.VerifiedFirmwarePackage
import java.io.FileInputStream
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
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
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class OwnerState(
    val connection: ConnectionState = ConnectionState(),
    val status: KitsuStatus? = null,
    val history: List<HistoryEntry> = emptyList(),
    val peers: List<Peer> = emptyList(),
    val messages: List<Message> = emptyList(),
    val channels: List<MeshChannel> = emptyList(),
    val savedKitsu: List<BondedCompanion> = emptyList(),
    val activeDeviceAddress: String? = null,
    val pendingPairing: BondedCompanion? = null,
    val pendingForgetAddress: String? = null,
    val loading: Boolean = false,
    val errorCode: String? = null,
    val pairing: Boolean = false,
    val pairingProgress: ControllerPairingProgress? = null,
    val meshConfigurationInFlight: Boolean = false,
)

class OwnerRepository(
    private val coordinator: ConnectionCoordinator,
    private val cache: EncryptedBoundedCache,
    private val credentials: CredentialStore,
    private val pairingService: ControllerPairingService,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate),
) {
    private val mutableState = MutableStateFlow(OwnerState())
    val state: StateFlow<OwnerState> = mutableState.asStateFlow()
    private var eventJob: Job? = null
    private var lastLiveNamespace: OwnerCursorNamespace? = null

    init {
        scope.launch {
            val cached = withContext(Dispatchers.IO) { cache.read() }
            val devices = credentials.bondedCompanions()
            val active = credentials.bondedCompanion()
            val pendingPairing = reconcilePendingPairing(devices)
            val pendingForget = reconcilePendingForget(devices)
            mutableState.value = mutableState.value.copy(
                status = cached?.status,
                history = cached?.history.orEmpty(),
                peers = cached?.peers.orEmpty(),
                messages = cached?.messages.orEmpty(),
                savedKitsu = devices,
                activeDeviceAddress = active?.deviceAddress,
                pendingPairing = pendingPairing,
                pendingForgetAddress = pendingForget,
            )
        }
        scope.launch {
            coordinator.state.collect { connection ->
                mutableState.value = mutableState.value.copy(connection = connection)
            }
        }
    }

    suspend fun connectAndRefresh(userInitiated: Boolean = false) {
        if (mutableState.value.pairing) return
        stopEvents()
        mutableState.value = mutableState.value.copy(loading = true, errorCode = null)
        val connection = coordinator.connect(userInitiated)
        if (!connection.connected) {
            mutableState.value = mutableState.value.copy(loading = false, errorCode = connection.detail)
            return
        }
        refresh()
        if (coordinator.state.value.connected) subscribeToEvents()
    }

    suspend fun selectDevice(deviceAddress: String) {
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
        )
    }

    suspend fun refresh() {
        try {
            val snapshot = coordinator.withTransport { transport ->
                val previous = mutableState.value
                val status = transport.status()
                val liveNamespace = OwnerCursorNamespace(ConnectionMode.DIRECT_BLE, status.deviceId)
                val history = transport.history(
                    after = OwnerCursorPolicy.resume(
                        previous.history.lastOrNull()?.cursor,
                        lastLiveNamespace,
                        liveNamespace,
                    ),
                    limit = 100,
                )
                val messages = transport.messages(
                    after = OwnerCursorPolicy.messagesAfter(),
                    limit = 100,
                )
                val peers = transport.peers()
                val channels = transport.channels()
                val replaceHistory = OwnerCursorPolicy.shouldReplace(
                    lastLiveNamespace,
                    liveNamespace,
                    history.cursorExpired,
                )
                lastLiveNamespace = liveNamespace
                previous.copy(
                    connection = coordinator.state.value,
                    status = status,
                    history = if (replaceHistory) history.items else
                        (previous.history + history.items).distinctBy { it.id }.takeLast(CachePolicy.MAX_HISTORY),
                    peers = peers.items,
                    messages = messages.items.takeLast(CachePolicy.MAX_MESSAGES),
                    channels = channels,
                    loading = false,
                    errorCode = null,
                )
            }
            mutableState.value = snapshot
            persistCache(snapshot)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            SafeLog.warn("owner_refresh", "refresh_failed", failure)
            mutableState.value = mutableState.value.copy(
                connection = coordinator.state.value,
                loading = false,
                errorCode = (failure as? TransportException)?.code ?: "refresh_failed",
            )
        }
    }

    suspend fun perform(command: ActionCommand): ActionReceipt =
        coordinator.withTransport { transport ->
            transport.action(command).also {
                mutableState.value = mutableState.value.copy(errorCode = null)
                refresh()
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

    /** Revokes this app's authenticated controller on Kitsu before deleting the local root. */
    suspend fun forgetController(deviceAddress: String) {
        val pending = credentials.pendingControllerForgetAddress()
        if (pending != null && !pending.equals(deviceAddress, ignoreCase = true)) {
            throw TransportException("controller_forget_pending")
        }
        val recoveringLostReceipt = pending?.equals(deviceAddress, ignoreCase = true) == true
        selectDevice(deviceAddress)

        var connection = coordinator.state.value
        if (!connection.connected || !coordinator.isConnectedTo(deviceAddress)) {
            connection = coordinator.connect(userInitiated = true)
        }
        if (!connection.connected) {
            if (recoveringLostReceipt && connection.detail == "controller_auth_failed") {
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
            if (verification.detail == "controller_auth_failed") {
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
            messages = emptyList(),
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
            messages = emptyList(),
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
        eventJob = scope.launch {
            runCatching {
                coordinator.withTransport { transport ->
                    transport.events(cursor).collect { refresh() }
                }
            }.onFailure { SafeLog.warn("owner_events", "event_stream_ended", it) }
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
                    writtenAt = System.currentTimeMillis() / 1_000L,
                ),
            )
        }.onFailure { SafeLog.warn("owner_cache", "cache_write_failed", it) }
    }

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

    private companion object {
        const val UNBOUND_UPDATE_ABORT_ID =
            "0000000000000000000000000000000000000000000000000000000000000000"
        val FORGET_OUTCOME_UNKNOWN = setOf("request_timeout", "disconnected", "gatt_disconnected")
        val TRANSIENT_UPDATE_FAILURES = setOf(
            "request_timeout", "disconnected", "gatt_disconnected", "gatt_write_failed",
        )
    }
}
