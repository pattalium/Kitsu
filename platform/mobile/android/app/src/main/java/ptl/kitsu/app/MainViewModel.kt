package ptl.kitsu.app

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.repository.OwnerRepository
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.transport.TransportException
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

class MainViewModel(application: Application) : AndroidViewModel(application) {
    private val services = (application as KitsuApplication).services
    private val repository: OwnerRepository = services.ownerRepository

    val owner: StateFlow<OwnerState> = repository.state
    private val mutableNotice = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = mutableNotice.asStateFlow()
    private val mutableFirmware = MutableStateFlow(FirmwareUpdateUiState())
    val firmware: StateFlow<FirmwareUpdateUiState> = mutableFirmware.asStateFlow()
    private var importedPackage: VerifiedFirmwarePackage? = null
    private var firmwareJob: Job? = null
    private var advertiseJob: Job? = null
    private var messageReadJob: Job? = null
    private val pendingMessageReads = linkedMapOf<String, LinkedHashSet<String>>()
    private val messageSubmission = MessageSubmissionCoordinator()
    val messageSubmissionInFlight: StateFlow<Boolean> = messageSubmission.inFlight

    init {
        // Exactly one bounded cold-start BLE attempt. A durable Disconnect suppresses it.
        viewModelScope.launch {
            delay(100)
            repository.connectAndRefresh(userInitiated = false)
        }
    }

    fun connect(deviceAddress: String? = owner.value.activeDeviceAddress) {
        if (rejectWhileFirmwareBusy()) return
        if (deviceAddress == null) {
            mutableNotice.value = "Pair a Kitsu first."
            return
        }
        viewModelScope.launch {
            runCatching { repository.connectDevice(deviceAddress) }.report("connect_failed")
        }
    }

    fun reconnectBluetooth() = connect()

    fun reportBlePermissionDenied(pairing: Boolean) {
        repository.reportLocalError(blePermissionErrorCode(pairing))
        mutableNotice.value = if (pairing) {
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

    private fun requestId(): String = UUID.randomUUID().toString()

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
            (this as? FirmwarePackageException)?.code ?: fallback

    private companion object {
        // Mark-read may wait behind a full mesh snapshot before its own signed
        // request starts. Do not cancel an active authenticated request sooner
        // than the transport can legitimately finish it.
        const val MESSAGE_READ_TIMEOUT_MILLIS = 65_000L
        const val MESSAGE_READ_RETRY_DELAY_MILLIS = 750L
        const val MESSAGE_READ_MAX_ATTEMPTS = 2
    }
}

internal fun blePermissionErrorCode(pairing: Boolean): String =
    if (pairing) "pairing_bluetooth_permission_required" else "bluetooth_permission_required"
