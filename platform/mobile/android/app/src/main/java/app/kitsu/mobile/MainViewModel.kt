package app.kitsu.mobile

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.pairing.PairingException
import app.kitsu.mobile.repository.OwnerRepository
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.transport.TransportException
import app.kitsu.mobile.update.FirmwareInstallProgress
import app.kitsu.mobile.update.FirmwareInstallStage
import app.kitsu.mobile.update.FirmwarePackageException
import app.kitsu.mobile.update.FirmwareUpdatePackageReader
import app.kitsu.mobile.update.VerifiedFirmwarePackage
import app.kitsu.mobile.update.locksCompanionControls
import java.io.File
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

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

    fun simpleAction(kind: ActionKind) {
        if (rejectWhileFirmwareBusy()) return
        val command = when (kind) {
            ActionKind.LISTEN_ONCE -> ActionCommand(kind, requestId(), durationMs = 15_000)
            ActionKind.PET, ActionKind.FEED, ActionKind.PLAY -> ActionCommand(kind, requestId())
            ActionKind.SEND_MESSAGE -> return
        }
        perform(command)
    }

    fun sendMessage(targetId: String, text: String, route: MessageRoute, onQueued: () -> Unit = {}) {
        if (rejectWhileFirmwareBusy()) return
        perform(ActionCommand(
            kind = ActionKind.SEND_MESSAGE,
            clientRequestId = requestId(),
            targetId = targetId.trim(),
            text = text,
            messageRoute = route,
        ), onQueued)
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
}
