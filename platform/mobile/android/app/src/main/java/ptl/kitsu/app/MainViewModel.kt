package ptl.kitsu.app

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.EncounterUnlockCode
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.repository.OwnerRepository
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.security.AndroidKeystoreEncounterCodeVault
import ptl.kitsu.app.security.EncounterCodeVault
import ptl.kitsu.app.security.EncounterCodeVaultException
import ptl.kitsu.app.security.SafeLog
import ptl.kitsu.app.transport.TransportException
import ptl.kitsu.app.transport.FirmwareEncounterApiPolicy
import ptl.kitsu.app.update.FirmwareInstallProgress
import ptl.kitsu.app.update.FirmwareInstallStage
import ptl.kitsu.app.update.FirmwarePackageException
import ptl.kitsu.app.update.FirmwareUpdatePackageReader
import ptl.kitsu.app.update.VerifiedFirmwarePackage
import ptl.kitsu.app.update.locksCompanionControls
import java.io.File
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

data class FirmwareUpdateUiState(
    val progress: FirmwareInstallProgress = FirmwareInstallProgress(),
    val importedReleaseId: String? = null,
    val updateId: String? = null,
)

data class EncounterUnlockUiState(
    val records: List<EncounterUnlockCode> = emptyList(),
    val loading: Boolean = true,
    val syncing: Boolean = false,
    val errorCode: String? = null,
)

class MainViewModel(application: Application) : AndroidViewModel(application) {
    private val services = (application as KitsuApplication).services
    private val repository: OwnerRepository = services.ownerRepository
    private val encounterVault: EncounterCodeVault = AndroidKeystoreEncounterCodeVault(application)

    val owner: StateFlow<OwnerState> = repository.state
    private val mutableNotice = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = mutableNotice.asStateFlow()
    private val mutableFirmware = MutableStateFlow(FirmwareUpdateUiState())
    val firmware: StateFlow<FirmwareUpdateUiState> = mutableFirmware.asStateFlow()
    private val mutableEncounterUnlocks = MutableStateFlow(EncounterUnlockUiState())
    val encounterUnlocks: StateFlow<EncounterUnlockUiState> = mutableEncounterUnlocks.asStateFlow()
    private val mutableNeighborActionsInFlight = MutableStateFlow<Set<String>>(emptySet())
    val neighborActionsInFlight: StateFlow<Set<String>> = mutableNeighborActionsInFlight.asStateFlow()
    private var importedPackage: VerifiedFirmwarePackage? = null
    private var firmwareJob: Job? = null
    private var advertiseJob: Job? = null
    private var messageReadJob: Job? = null
    private val pendingMessageReads = linkedMapOf<String, LinkedHashSet<String>>()
    private val messageSubmission = MessageSubmissionCoordinator()
    val messageSubmissionInFlight: StateFlow<Boolean> = messageSubmission.inFlight

    init {
        // The vault is independent from selected-device state and is hydrated before BLE sync.
        viewModelScope.launch {
            loadEncounterVault()
            // Exactly one bounded cold-start BLE attempt. A durable Disconnect suppresses it.
            delay(100)
            val connected = runCatching { repository.connectAndRefresh(userInitiated = false) }
            if (connected.isSuccess && owner.value.connection.connected) {
                syncEncounterCodesInternal(silent = true)
            }
        }
    }

    fun connect(deviceAddress: String? = owner.value.activeDeviceAddress) {
        if (rejectWhileFirmwareBusy()) return
        if (deviceAddress == null) {
            mutableNotice.value = "Pair a Kitsu first."
            return
        }
        viewModelScope.launch {
            val result = runCatching { repository.connectDevice(deviceAddress) }
            result.report("connect_failed")
            if (result.isSuccess) syncEncounterCodesInternal(silent = true)
        }
    }

    fun reconnectBluetooth() = connect()

    fun reportBlePermissionDenied(pairing: Boolean, repair: Boolean = false) {
        repository.reportLocalError(blePermissionErrorCode(pairing, repair))
        mutableNotice.value = if (repair) {
            "Bluetooth permission is required to repair this saved pairing."
        } else if (pairing) {
            "Bluetooth permission is required to pair this phone."
        } else {
            "Bluetooth permission is required to connect."
        }
    }

    fun selectDevice(deviceAddress: String) = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.selectDevice(deviceAddress) }.report("select_failed")
    }

    fun disconnect() {
        if (rejectWhileFirmwareBusy()) return
        repository.recordUserDisconnectIntent()
        viewModelScope.launch {
            runCatching { repository.disconnectByUser() }.report("disconnect_failed")
        }
    }

    fun refresh() = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.refresh() }.report("refresh_failed")
    }

    fun refreshMessages() = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.refreshMessagesOnly() }.report("messages_refresh_failed")
    }

    fun synchronizeClock() = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.synchronizeClock() }.report("clock_sync_failed")
    }

    fun simpleAction(kind: ActionKind) {
        if (rejectWhileFirmwareBusy()) return
        val command = when (kind) {
            ActionKind.LISTEN_ONCE -> ActionCommand(kind, requestId(), durationMs = 15_000)
            ActionKind.PET, ActionKind.FEED, ActionKind.PLAY -> ActionCommand(kind, requestId())
            ActionKind.ADVERTISE_ONCE, ActionKind.SEND_MESSAGE -> return
        }
        perform(command)
    }

    fun advertiseOnce(scope: AdvertiseScope) {
        if (rejectWhileFirmwareBusy() || advertiseJob?.isActive == true) return
        val current = owner.value
        if (!current.connection.connected) {
            mutableNotice.value = "Connect your Kitsu before advertising."
            return
        }
        val mesh = current.status?.mesh
        if (mesh?.advertiseSupported != true) {
            mutableNotice.value = "This Kitsu firmware does not support signed advertising."
            return
        }
        if (!mesh.advertiseReady) {
            mutableNotice.value = mesh.advertiseError ?: "Mesh advertising is not ready."
            return
        }
        val command = ActionCommand(
            kind = ActionKind.ADVERTISE_ONCE,
            clientRequestId = requestId(),
            advertiseScope = scope,
        )
        advertiseJob = viewModelScope.launch {
            try {
                repository.advertiseOnce(command)
                mutableNotice.value = if (scope == AdvertiseScope.NEARBY) {
                    "Nearby advertisement queued."
                } else {
                    "Mesh-wide advertisement queued."
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                mutableNotice.value = failure.codeOr("advertise_failed")
            }
        }
    }

    fun sendMessage(targetId: String, text: String, route: MessageRoute, onQueued: () -> Unit = {}) {
        if (rejectWhileFirmwareBusy()) return
        viewModelScope.launch {
            messageSubmission.runOnce {
                val result = runCatching {
                    repository.perform(ActionCommand(
                        kind = ActionKind.SEND_MESSAGE,
                        clientRequestId = requestId(),
                        targetId = targetId.trim(),
                        text = text,
                        messageRoute = route,
                    ))
                }
                if (result.isSuccess) onQueued()
                result.report("action_failed")
            }
        }
    }

    /** Coalesces by immutable journal session so rebooted raw IDs can never alias. */
    fun markMessagesRead(expectedSession: String, messageIds: List<String>) {
        if (messageIds.isEmpty()) return
        pendingMessageReads.getOrPut(expectedSession, ::linkedSetOf).addAll(messageIds)
        if (messageReadJob?.isActive == true) return
        messageReadJob = viewModelScope.launch {
            while (pendingMessageReads.isNotEmpty()) {
                val (session, ids) = pendingMessageReads.entries.first()
                val batch = ids.take(24)
                ids.removeAll(batch.toSet())
                if (ids.isEmpty()) pendingMessageReads.remove(session)
                var attempt = 0
                var finished = false
                while (!finished && attempt < MESSAGE_READ_MAX_ATTEMPTS) {
                    attempt += 1
                    try {
                        withTimeout(MESSAGE_READ_TIMEOUT_MILLIS) {
                            repository.markMessagesRead(session, batch)
                        }
                        finished = true
                    } catch (_: TimeoutCancellationException) {
                        repository.reportMessageReadFailure(session, "message_read_timeout")
                        if (attempt < MESSAGE_READ_MAX_ATTEMPTS) delay(MESSAGE_READ_RETRY_DELAY_MILLIS)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        val terminal = failure.codeOr("message_read_failed") in setOf(
                            "firmware_operation_unavailable",
                            "message_read_session_changed",
                            "message_read_target_stale",
                            "journal_session_mismatch",
                            "snapshot_changed",
                            "message_not_inbound",
                        )
                        if (!terminal && attempt < MESSAGE_READ_MAX_ATTEMPTS) {
                            delay(MESSAGE_READ_RETRY_DELAY_MILLIS)
                        } else {
                            finished = true
                        }
                    }
                }
            }
        }
    }

    fun configureMesh(enabled: Boolean) = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.configureMesh(enabled) }.report("mesh_configuration_failed")
    }

    fun syncEncounterCodes() = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        syncEncounterCodesInternal(silent = false)
    }

    /** Explicit deletion only. Device selection, Disconnect, and Forget never clear this vault. */
    fun deleteEncounterCodesForDevice(deviceId: String) = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        val result = runCatching {
            withContext(Dispatchers.IO) { encounterVault.deleteForDevice(deviceId) }
        }
        result.onSuccess { records ->
            mutableEncounterUnlocks.value = mutableEncounterUnlocks.value.copy(
                records = records,
                loading = false,
                errorCode = null,
            )
            mutableNotice.value = "Saved unlocks for $deviceId were deleted from this phone."
        }.onFailure { failure ->
            SafeLog.warn("encounter_vault", failure.codeOr("encounter_vault_delete_failed"), failure)
            mutableNotice.value = failure.codeOr("encounter_vault_delete_failed")
        }
    }

    /** Targeted neighbor care never passes through local companion actions. */
    fun interactWithNeighbor(neighbor: NearbyKitsu, kind: NeighborInteractionKind) {
        if (rejectWhileFirmwareBusy()) return
        if (kind !in owner.value.nearbyInteractionKinds) {
            mutableNotice.value = "${kind.displayName()} is not supported by this Kitsu firmware yet."
            return
        }
        val live = owner.value.nearbyKitsu.firstOrNull {
            it.deviceId == neighbor.deviceId &&
                it.sessionNonce == neighbor.sessionNonce &&
                it.nextSequence == neighbor.nextSequence
        }
        if (live == null || !owner.value.connection.connected) {
            mutableNotice.value = "That Kitsu is no longer nearby. Listen again to refresh nearby Kitsu."
            return
        }
        if (live.sessionKey in mutableNeighborActionsInFlight.value) return
        mutableNeighborActionsInFlight.value += live.sessionKey
        viewModelScope.launch {
            val command = NeighborInteractionCommand(
                actionId = requestId(),
                targetDeviceId = live.deviceId,
                targetSessionNonce = live.sessionNonce,
                sequence = live.nextSequence,
                kind = kind,
                expiresAtEpoch = java.time.Instant.now().epochSecond + 30L,
            )
            try {
                val result = runCatching { repository.interactWithNeighbor(command) }
                if (result.isSuccess) mutableNotice.value = kind.acceptedNotice()
                result.report("neighbor_interaction_failed")
            } finally {
                mutableNeighborActionsInFlight.value -= live.sessionKey
            }
        }
    }

    fun petNeighbor(neighbor: NearbyKitsu) =
        interactWithNeighbor(neighbor, NeighborInteractionKind.PET)

    fun pairController(label: String) {
        if (rejectWhileFirmwareBusy()) return
        viewModelScope.launch {
            runCatching { repository.pairController(label.trim().ifBlank { "My phone" }) }
                .report("pairing_failed")
        }
    }

    fun finishPendingPairing() {
        if (rejectWhileFirmwareBusy()) return
        viewModelScope.launch {
            val result = runCatching { repository.finishPendingPairing() }
            val failure = result.exceptionOrNull()
            if ((failure as? PairingException)?.code == "pairing_not_completed_repair_required") {
                mutableNotice.value = "Pairing was not completed. Hold PRG and pair this phone again."
            } else {
                result.report("pairing_recovery_failed")
            }
        }
    }

    fun cancelPairing() {
        if (rejectWhileFirmwareBusy()) return
        repository.cancelPairing()
    }

    fun repairBluetoothPairing(deviceAddress: String? = owner.value.activeDeviceAddress) {
        if (rejectWhileFirmwareBusy()) return
        if (deviceAddress == null) {
            mutableNotice.value = "Select a saved Kitsu before repairing Bluetooth pairing."
            return
        }
        viewModelScope.launch {
            val result = runCatching { repository.repairBluetoothPairing(deviceAddress) }
            val code = (result.exceptionOrNull() as? PairingException)?.code
            mutableNotice.value = when (code) {
                BluetoothPairingRepairPolicy.ANDROID_FORGET_REQUIRED ->
                    "Android still has the old bond. Open Bluetooth settings, forget this Kitsu, then continue repair."
                BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING ->
                    "The Bluetooth bond is repaired, but Kitsu no longer has this saved controller authorization."
                "repair_device_absent" ->
                    "Kitsu was not found. Open Pair Phone on Kitsu, keep it close, then retry."
                "secure_bond_failed", "secure_bond_verification_failed" ->
                    "Android did not finish secure pairing. Open Pair Phone on Kitsu and retry."
                null -> if (result.isSuccess) {
                    "Bluetooth pairing repaired. Saved controller kept."
                } else {
                    "Bluetooth pairing repair failed. No saved controller or app data was changed."
                }
                else -> code
            }
        }
    }

    fun cancelBluetoothPairingRepair() {
        if (rejectWhileFirmwareBusy()) return
        repository.cancelBluetoothPairingRepair()
    }

    fun forgetController(deviceAddress: String) = viewModelScope.launch {
        if (rejectWhileFirmwareBusy()) return@launch
        runCatching { repository.forgetController(deviceAddress) }.report("controller_forget_failed")
    }

    fun importFirmware(uri: Uri) = viewModelScope.launch {
        firmwareJob?.cancel()
        val old = importedPackage
        importedPackage = null
        old?.imageFile?.delete()
        mutableFirmware.value = FirmwareUpdateUiState(
            progress = FirmwareInstallProgress(FirmwareInstallStage.PREPARING),
        )
        try {
            val verified = withContext(Dispatchers.IO) {
                val destination = File(getApplication<Application>().cacheDir, "imported-${System.nanoTime()}.esp32.bin")
                val input = getApplication<Application>().contentResolver.openInputStream(uri)
                    ?: throw FirmwarePackageException("package_open_failed")
                input.use { FirmwareUpdatePackageReader.read(it, destination) }
            }
            importedPackage = verified
            mutableFirmware.value = FirmwareUpdateUiState(
                progress = FirmwareInstallProgress(
                    stage = FirmwareInstallStage.IMPORTED,
                    firmwareVersion = verified.manifest.firmwareVersion,
                    imageBytes = verified.manifest.imageBytes,
                ),
                importedReleaseId = verified.manifest.releaseId,
                updateId = verified.updateId,
            )
        } catch (failure: Throwable) {
            mutableFirmware.value = FirmwareUpdateUiState(
                progress = FirmwareInstallProgress(
                    stage = FirmwareInstallStage.FAILED,
                    errorCode = failure.codeOr("firmware_import_failed"),
                ),
            )
        }
    }

    fun installImportedFirmware() {
        val selected = importedPackage ?: run {
            mutableNotice.value = "Choose a signed .kitsu-fw file first."
            return
        }
        if (!owner.value.connection.connected) {
            mutableNotice.value = "Connect the selected Kitsu before installing."
            return
        }
        if (mutableFirmware.value.progress.stage.locksCompanionControls) return
        mutableFirmware.value = mutableFirmware.value.copy(
            progress = FirmwareInstallProgress(
                stage = FirmwareInstallStage.PREPARING,
                firmwareVersion = selected.manifest.firmwareVersion,
                imageBytes = selected.manifest.imageBytes,
            ),
        )
        firmwareJob?.cancel()
        firmwareJob = viewModelScope.launch {
            try {
                repository.installFirmware(selected) { progress ->
                    mutableFirmware.value = mutableFirmware.value.copy(progress = progress)
                }
            } catch (cancelled: CancellationException) {
                val aborted = withContext(NonCancellable) {
                    runCatching { repository.abortFirmwareUpdate(selected.updateId) }.isSuccess
                }
                mutableFirmware.value = mutableFirmware.value.copy(
                    progress = mutableFirmware.value.progress.copy(
                        stage = if (aborted) FirmwareInstallStage.IMPORTED else FirmwareInstallStage.FAILED,
                        errorCode = if (aborted) null else "firmware_abort_failed",
                    ),
                )
                throw cancelled
            } catch (failure: Throwable) {
                mutableFirmware.value = mutableFirmware.value.copy(
                    progress = mutableFirmware.value.progress.copy(
                        stage = FirmwareInstallStage.FAILED,
                        errorCode = failure.codeOr("firmware_update_failed"),
                    ),
                )
            }
        }
    }

    fun cancelFirmwareUpdate() {
        firmwareJob?.cancel()
        firmwareJob = null
    }

    fun resetInterruptedFirmwareUpdate() = viewModelScope.launch {
        try {
            repository.resetInterruptedFirmwareUpdate()
            val selected = importedPackage
            mutableFirmware.value = if (selected == null) FirmwareUpdateUiState() else
                mutableFirmware.value.copy(
                    progress = FirmwareInstallProgress(
                        FirmwareInstallStage.IMPORTED,
                        selected.manifest.firmwareVersion,
                        imageBytes = selected.manifest.imageBytes,
                    ),
                )
        } catch (failure: Throwable) {
            mutableNotice.value = failure.codeOr("firmware_update_reset_failed")
        }
    }

    fun clearNotice() {
        mutableNotice.value = null
    }

    fun showNotice(message: String) {
        mutableNotice.value = message
    }

    override fun onCleared() {
        importedPackage?.imageFile?.delete()
        super.onCleared()
    }

    private fun perform(command: ActionCommand, onSuccess: () -> Unit = {}) = viewModelScope.launch {
        val result = runCatching { repository.perform(command) }
        if (result.isSuccess) onSuccess()
        result.report("action_failed")
    }

    private suspend fun loadEncounterVault() {
        val result = runCatching { withContext(Dispatchers.IO) { encounterVault.read() } }
        result.onSuccess { records ->
            mutableEncounterUnlocks.value = EncounterUnlockUiState(records = records, loading = false)
        }.onFailure { failure ->
            SafeLog.warn("encounter_vault", failure.codeOr("encounter_vault_read_failed"), failure)
            mutableEncounterUnlocks.value = EncounterUnlockUiState(
                loading = false,
                errorCode = failure.codeOr("encounter_vault_read_failed"),
            )
        }
    }

    private suspend fun syncEncounterCodesInternal(silent: Boolean) {
        if (mutableEncounterUnlocks.value.syncing) return
        if (!owner.value.connection.connected) {
            if (!silent) mutableNotice.value = "Connect your Kitsu to sync saved unlocks."
            return
        }
        if (!FirmwareEncounterApiPolicy.supportsV1(owner.value.status?.firmwareVersion)) {
            if (!silent) mutableNotice.value = "Update Kitsu firmware to sync encounter unlocks."
            return
        }
        mutableEncounterUnlocks.value = mutableEncounterUnlocks.value.copy(
            syncing = true,
            errorCode = null,
        )
        val result = runCatching {
            val fromKitsu = repository.loadEncounterCodes()
            withContext(Dispatchers.IO) { encounterVault.upsert(fromKitsu) }
        }
        result.onSuccess { records ->
            mutableEncounterUnlocks.value = EncounterUnlockUiState(records = records, loading = false)
            if (!silent) mutableNotice.value = "Saved unlocks are up to date."
        }.onFailure { failure ->
            val code = failure.codeOr("encounter_codes_sync_failed")
            if (code != "firmware_operation_unavailable") {
                SafeLog.warn("encounter_sync", code, failure)
            }
            mutableEncounterUnlocks.value = mutableEncounterUnlocks.value.copy(
                loading = false,
                syncing = false,
                errorCode = code,
            )
            if (!silent) {
                mutableNotice.value = if (code == "firmware_operation_unavailable") {
                    "This firmware does not expose encounter unlocks yet."
                } else code
            }
        }
    }

    private fun requestId(): String = UUID.randomUUID().toString()

    private fun NeighborInteractionKind.displayName(): String = when (this) {
        NeighborInteractionKind.PET -> "Pet"
        NeighborInteractionKind.GREET -> "Greet"
        NeighborInteractionKind.PLAY -> "Play"
        NeighborInteractionKind.GIFT -> "Gift"
    }

    private fun NeighborInteractionKind.acceptedNotice(): String = when (this) {
        NeighborInteractionKind.PET -> "Pet is queued for the nearby Kitsu."
        NeighborInteractionKind.GREET -> "Greeting is queued for the nearby Kitsu."
        NeighborInteractionKind.PLAY -> "Playtime is queued for the nearby Kitsu."
        NeighborInteractionKind.GIFT -> "Gift is queued for the nearby Kitsu."
    }

    private fun rejectWhileFirmwareBusy(): Boolean {
        if (!mutableFirmware.value.progress.stage.locksCompanionControls) return false
        mutableNotice.value = "Firmware update in progress. Other controls are locked."
        return true
    }

    private fun Result<*>.report(fallback: String) {
        exceptionOrNull()?.let { mutableNotice.value = it.codeOr(fallback) }
    }

    private fun Throwable.codeOr(fallback: String): String =
        (this as? TransportException)?.code ?: (this as? PairingException)?.code ?:
            (this as? FirmwarePackageException)?.code ?: (this as? EncounterCodeVaultException)?.code ?:
            fallback

    private companion object {
        // Mark-read may wait behind a full mesh snapshot before its own signed
        // request starts. Do not cancel an active authenticated request sooner
        // than the transport can legitimately finish it.
        const val MESSAGE_READ_TIMEOUT_MILLIS = 65_000L
        const val MESSAGE_READ_RETRY_DELAY_MILLIS = 750L
        const val MESSAGE_READ_MAX_ATTEMPTS = 2
    }
}

internal fun blePermissionErrorCode(pairing: Boolean, repair: Boolean = false): String = when {
    repair -> BluetoothPairingRepairPolicy.PERMISSION_REQUIRED
    pairing -> "pairing_bluetooth_permission_required"
    else -> "bluetooth_permission_required"
}
