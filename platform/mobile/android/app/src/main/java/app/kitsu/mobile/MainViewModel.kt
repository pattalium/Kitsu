package app.kitsu.mobile

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.AdvertiseScope
import app.kitsu.mobile.model.LocationExposure
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiSecurity
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.repository.GatewayEnrollmentStage
import app.kitsu.mobile.ui.OwnerAccountStatus
import java.util.UUID
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.Job
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

class MainViewModel(application: Application) : AndroidViewModel(application) {
    private val services = (application as KitsuApplication).services
    private val repository = services.ownerRepository
    val state: StateFlow<OwnerState> = repository.state
    val mobileRelayState = services.mobileRelayController.state

    private val mutableNotice = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = mutableNotice.asStateFlow()
    private var pairingJob: Job? = null
    private var connectionJob: Job? = null
    private var disconnectJob: Job? = null
    private var provisioningJob: Job? = null
    private var enrollmentJob: Job? = null

    sealed interface MessageSendState {
        data object Idle : MessageSendState
        data object Sending : MessageSendState
        data class Accepted(val actionId: String) : MessageSendState
        data class Failed(val code: String) : MessageSendState
    }

    sealed interface ProvisioningState {
        data object Idle : ProvisioningState
        data object Saving : ProvisioningState
        data object Stored : ProvisioningState
        data class AcceptedUnverified(val code: String) : ProvisioningState
        data class Failed(val code: String) : ProvisioningState
    }

    sealed interface WifiRetryState {
        data object Idle : WifiRetryState
        data object Retrying : WifiRetryState
        data object Accepted : WifiRetryState
        data class Failed(val code: String) : WifiRetryState
    }

    sealed interface GatewayEnrollmentState {
        data object Idle : GatewayEnrollmentState
        data object CreatingClaim : GatewayEnrollmentState
        data class WaitingForPhysicalConfirmation(val remainingMilliseconds: Int) : GatewayEnrollmentState
        data object SwitchingToWifi : GatewayEnrollmentState
        data object PollingBackend : GatewayEnrollmentState
        data object Complete : GatewayEnrollmentState
        data object CancelledBeforePhysicalConfirmation : GatewayEnrollmentState
        data object MonitoringStoppedAfterPhysicalCommit : GatewayEnrollmentState
        data class Failed(
            val code: String,
            val physicalCommitAccepted: Boolean = false,
        ) : GatewayEnrollmentState
    }

    private val mutableMessageSendState = MutableStateFlow<MessageSendState>(MessageSendState.Idle)
    val messageSendState: StateFlow<MessageSendState> = mutableMessageSendState.asStateFlow()
    private val mutableWifiProvisioningState = MutableStateFlow<ProvisioningState>(ProvisioningState.Idle)
    val wifiProvisioningState: StateFlow<ProvisioningState> = mutableWifiProvisioningState.asStateFlow()
    private val mutableWifiRetryState = MutableStateFlow<WifiRetryState>(WifiRetryState.Idle)
    val wifiRetryState: StateFlow<WifiRetryState> = mutableWifiRetryState.asStateFlow()
    private val mutableGatewayProvisioningState = MutableStateFlow<ProvisioningState>(ProvisioningState.Idle)
    val gatewayProvisioningState: StateFlow<ProvisioningState> = mutableGatewayProvisioningState.asStateFlow()
    private val mutableGatewayEnrollmentState = MutableStateFlow<GatewayEnrollmentState>(GatewayEnrollmentState.Idle)
    val gatewayEnrollmentState: StateFlow<GatewayEnrollmentState> = mutableGatewayEnrollmentState.asStateFlow()
    private val mutableOwnerAccountStatus = MutableStateFlow(OwnerAccountStatus.CHECKING)
    val ownerAccountStatus: StateFlow<OwnerAccountStatus> = mutableOwnerAccountStatus.asStateFlow()

    init {
        refreshOwnerAccountStatus()
        viewModelScope.launch {
            services.mobileRelayController.awaitInitialized()
            if (!mobileRelayOwnsBluetooth()) {
                connect(userInitiated = false)
            }
        }
    }

    fun refreshOwnerAccountStatus() = viewModelScope.launch {
        mutableOwnerAccountStatus.value = runCatching { services.oidc.hasStoredSession() }
            .fold(
                onSuccess = { if (it) OwnerAccountStatus.SIGNED_IN else OwnerAccountStatus.SIGNED_OUT },
                onFailure = { OwnerAccountStatus.SIGNED_OUT },
            )
    }

    fun markOwnerSignedIn() {
        mutableOwnerAccountStatus.value = OwnerAccountStatus.SIGNED_IN
    }

    fun markOwnerSignedOut() {
        mutableOwnerAccountStatus.value = OwnerAccountStatus.SIGNED_OUT
    }

    fun reconnect() {
        if (mobileRelayOwnsBluetooth()) {
            mutableNotice.value = "Disconnect the public gateway before using owner connections"
            return
        }
        if (deviceSetupInProgress()) {
            mutableNotice.value = "Finish the current device setup step before switching connections"
            return
        }
        connect(userInitiated = true)
    }

    fun reconnectBluetooth() {
        if (mobileRelayOwnsBluetooth()) {
            mutableNotice.value = "Disconnect the public gateway before using nearby Bluetooth"
            return
        }
        if (deviceSetupInProgress()) {
            mutableNotice.value = "Finish the current device setup step before switching connections"
            return
        }
        connect(userInitiated = true, directOnly = true)
    }

    fun reconnectRemote() {
        if (mobileRelayOwnsBluetooth()) {
            mutableNotice.value = "Disconnect the public gateway before using owner connections"
            return
        }
        if (connectionJob?.isActive == true && disconnectJob?.isActive != true) return
        if (deviceSetupInProgress()) {
            mutableNotice.value = "Finish the current device setup step before switching connections"
            return
        }
        val pendingDisconnect = disconnectJob
        connectionJob = viewModelScope.launch {
            pendingDisconnect?.join()
            try {
                repository.connectRemoteAndRefresh()
                refreshOwnerAccountStatus()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Throwable) {
                refreshOwnerAccountStatus()
                mutableNotice.value = "Remote connection failed"
            }
        }
    }

    fun reconnectIfAllowed() = connect(userInitiated = false)

    private fun connect(userInitiated: Boolean, directOnly: Boolean = false) {
        if (mobileRelayOwnsBluetooth()) {
            mutableNotice.value = "Public gateway is already using the Kitsu Bluetooth connection"
            return
        }
        if (connectionJob?.isActive == true && disconnectJob?.isActive != true) return
        val pendingDisconnect = disconnectJob
        connectionJob = viewModelScope.launch {
            pendingDisconnect?.join()
            try {
                if (directOnly) {
                    repository.connectDirectAndRefresh()
                } else {
                    repository.connectAndRefresh(userInitiated)
                }
                refreshOwnerAccountStatus()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Throwable) {
                refreshOwnerAccountStatus()
                mutableNotice.value = "Connection failed"
            }
        }
    }

    fun disconnect() {
        val currentEnrollment = mutableGatewayEnrollmentState.value
        val enrollmentOutcome = when (currentEnrollment) {
            GatewayEnrollmentState.SwitchingToWifi,
            GatewayEnrollmentState.PollingBackend ->
                GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit
            GatewayEnrollmentState.Complete -> GatewayEnrollmentState.Complete
            is GatewayEnrollmentState.Failed -> if (currentEnrollment.physicalCommitAccepted) {
                currentEnrollment
            } else {
                GatewayEnrollmentState.Idle
            }
            else -> GatewayEnrollmentState.Idle
        }
        val message = when (enrollmentOutcome) {
            GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit,
            is GatewayEnrollmentState.Failed ->
                "Disconnected. Kitsu already accepted enrollment and may continue its Wi-Fi bootstrap."
            GatewayEnrollmentState.Complete ->
                "Disconnected. Enrollment remains complete; Bluetooth remains on."
            else -> "Disconnected. Bluetooth remains on."
        }
        disconnect(enrollmentOutcome, message)
    }

    private fun disconnect(
        enrollmentOutcome: GatewayEnrollmentState,
        successNotice: String,
    ) {
        if (disconnectJob?.isActive == true) return
        // Persist synchronously before jobs are cancelled or asynchronous teardown is launched.
        val disconnectChoiceSaved = repository.recordUserDisconnectIntent()
        mutableGatewayEnrollmentState.value = enrollmentOutcome
        val pendingConnection = connectionJob
        val pendingProvisioning = provisioningJob
        connectionJob?.cancel(CancellationException("user_disconnected"))
        pairingJob?.cancel(CancellationException("user_disconnected"))
        enrollmentJob?.cancel(CancellationException("user_disconnected"))
        repository.cancelPairing()
        disconnectJob = viewModelScope.launch {
            pendingConnection?.join()
            // A provisioning receipt and its immediately following signed
            // state read form one atomic user-visible result. Do not tear down
            // BLE between them; finish that bounded exchange, then disconnect.
            pendingProvisioning?.join()
            repository.disconnectByUser()
            mutableWifiProvisioningState.value = ProvisioningState.Idle
            mutableWifiRetryState.value = WifiRetryState.Idle
            mutableGatewayProvisioningState.value = ProvisioningState.Idle
            mutableGatewayEnrollmentState.value = enrollmentOutcome
            mutableNotice.value = if (disconnectChoiceSaved) {
                successNotice
            } else {
                "$successNotice Android could not save the stay-disconnected choice."
            }
        }
    }

    fun refresh() = viewModelScope.launch {
        runCatching { repository.refresh() }
            .onFailure { mutableNotice.value = "Refresh failed" }
    }

    fun simpleAction(kind: ActionKind) = perform(
        when (kind) {
            ActionKind.LISTEN_ONCE -> ActionCommand(kind, requestId(), durationMs = 60_000)
            ActionKind.ADVERTISE_ONCE -> ActionCommand(kind, requestId(), advertiseScope = AdvertiseScope.NEARBY)
            else -> ActionCommand(kind, requestId())
        },
    )

    fun sendMessage(targetId: String, text: String, route: MessageRoute) {
        if (mutableMessageSendState.value == MessageSendState.Sending) return
        val command = ActionCommand(
            ActionKind.SEND_MESSAGE,
            requestId(),
            targetId = targetId.trim(),
            text = text,
            messageRoute = route,
        )
        mutableMessageSendState.value = MessageSendState.Sending
        viewModelScope.launch {
            runCatching { repository.perform(command) }
                .onSuccess { receipt ->
                    if (receipt.accepted) {
                        mutableMessageSendState.value = MessageSendState.Accepted(receipt.clientRequestId)
                        mutableNotice.value = "Send accepted; awaiting mesh status"
                    } else {
                        val code = receipt.errorCode ?: "message_rejected"
                        mutableMessageSendState.value = MessageSendState.Failed(code)
                        mutableNotice.value = "Message rejected"
                    }
                }
                .onFailure { failure ->
                    val code = (failure as? app.kitsu.mobile.transport.TransportException)?.code
                        ?: "message_failed"
                    mutableMessageSendState.value = MessageSendState.Failed(code)
                    mutableNotice.value = "Message not sent"
                }
        }
    }

    fun shareLocationOnce(latE6: Int, lonE6: Int) = perform(
        ActionCommand(
            ActionKind.SHARE_LOCATION_ONCE,
            requestId(),
            latE6 = latE6,
            lonE6 = lonE6,
            locationExposure = LocationExposure.NEARBY_ADVERT,
        ),
    )

    fun provisionWifi(ssid: String, password: String, security: WifiSecurity) {
        if (enrollmentJob?.isActive == true) {
            mutableNotice.value = "Finish enrollment before changing device configuration"
            return
        }
        if (provisioningJob?.isActive == true) return
        provisioningJob = viewModelScope.launch {
            mutableWifiRetryState.value = WifiRetryState.Idle
            mutableWifiProvisioningState.value = ProvisioningState.Saving
            try {
                runCatching { repository.provisionWifi(WifiProvisioning(ssid, password, security)) }
                    .onSuccess {
                        mutableWifiProvisioningState.value = ProvisioningState.Stored
                        mutableNotice.value =
                            "Wi-Fi stored and verified. Kitsu is applying it; gateway enrollment enables remote access."
                    }
                    .onFailure { failure ->
                        if (failure is CancellationException) throw failure
                        val code = (failure as? app.kitsu.mobile.transport.TransportException)?.code
                            ?: "wifi_configuration_failed"
                        if (code in WIFI_ACCEPTED_VERIFICATION_ERRORS) {
                            mutableWifiProvisioningState.value = ProvisioningState.AcceptedUnverified(code)
                            mutableNotice.value =
                                "Kitsu accepted the Wi-Fi write, but Android could not verify stored state."
                        } else {
                            mutableWifiProvisioningState.value = ProvisioningState.Failed(code)
                            mutableNotice.value = "Wi-Fi was not verified as stored"
                        }
                    }
            } finally {
                provisioningJob = null
            }
        }
    }

    fun retryWifi() {
        if (enrollmentJob?.isActive == true) {
            mutableNotice.value = "Finish enrollment before retrying Wi-Fi"
            return
        }
        if (provisioningJob?.isActive == true) return
        provisioningJob = viewModelScope.launch {
            mutableWifiRetryState.value = WifiRetryState.Retrying
            try {
                repository.retryWifi()
                mutableWifiRetryState.value = WifiRetryState.Accepted
                mutableNotice.value = "Kitsu started a fresh Wi-Fi connection attempt"
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                val code = (failure as? app.kitsu.mobile.transport.TransportException)?.code
                    ?: "wifi_retry_failed"
                mutableWifiRetryState.value = WifiRetryState.Failed(code)
                mutableNotice.value = "Kitsu could not restart Wi-Fi"
            } finally {
                provisioningJob = null
            }
        }
    }

    fun configureGateway(configuration: GatewayConfiguration) {
        if (enrollmentJob?.isActive == true) {
            mutableNotice.value = "Finish enrollment before changing device configuration"
            return
        }
        if (provisioningJob?.isActive == true) return
        provisioningJob = viewModelScope.launch {
            mutableGatewayProvisioningState.value = ProvisioningState.Saving
            try {
                runCatching { repository.configureGateway(configuration) }
                    .onSuccess {
                        mutableGatewayProvisioningState.value = ProvisioningState.Stored
                        mutableNotice.value = "Gateway trust stored on Kitsu"
                    }
                    .onFailure { failure ->
                        if (failure is CancellationException) throw failure
                        val code = (failure as? app.kitsu.mobile.transport.TransportException)?.code
                            ?: "gateway_configuration_failed"
                        mutableGatewayProvisioningState.value = ProvisioningState.Failed(code)
                        mutableNotice.value = "Gateway configuration was not stored"
                    }
            } finally {
                provisioningJob = null
            }
        }
    }

    fun enrollGateway(displayName: String) {
        if (provisioningJob?.isActive == true) {
            mutableNotice.value = "Finish saving device configuration before enrollment"
            return
        }
        if (enrollmentJob?.isActive == true) return
        val normalizedName = displayName.trim()
        if (normalizedName.toByteArray(Charsets.UTF_8).size !in 1..80 ||
            normalizedName.any(Char::isISOControl)
        ) {
            mutableGatewayEnrollmentState.value = GatewayEnrollmentState.Failed("invalid_display_name")
            return
        }
        enrollmentJob = viewModelScope.launch {
            try {
                repository.enrollGateway(normalizedName) { progress ->
                    mutableGatewayEnrollmentState.value = when (progress.stage) {
                        GatewayEnrollmentStage.CREATING_CLAIM -> GatewayEnrollmentState.CreatingClaim
                        GatewayEnrollmentStage.WAITING_FOR_PHYSICAL_CONFIRMATION ->
                            GatewayEnrollmentState.WaitingForPhysicalConfirmation(
                                progress.remainingMilliseconds ?: 0,
                            )
                        GatewayEnrollmentStage.SWITCHING_TO_WIFI -> GatewayEnrollmentState.SwitchingToWifi
                        GatewayEnrollmentStage.POLLING_BACKEND -> GatewayEnrollmentState.PollingBackend
                        GatewayEnrollmentStage.COMPLETE -> GatewayEnrollmentState.Complete
                    }
                }
                mutableNotice.value = "Kitsu enrolled and connected through the owner service"
            } catch (cancelled: CancellationException) {
                val current = mutableGatewayEnrollmentState.value
                mutableGatewayEnrollmentState.value = when {
                    current == GatewayEnrollmentState.CancelledBeforePhysicalConfirmation -> current
                    current == GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit -> current
                    current.physicalCommitAccepted() ->
                        GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit
                    else -> GatewayEnrollmentState.Idle
                }
                throw cancelled
            } catch (failure: Throwable) {
                val physicalCommitAccepted =
                    mutableGatewayEnrollmentState.value.physicalCommitAccepted()
                val code = (failure as? app.kitsu.mobile.transport.TransportException)?.code
                    ?: "enrollment_failed"
                mutableGatewayEnrollmentState.value = GatewayEnrollmentState.Failed(
                    code,
                    physicalCommitAccepted = physicalCommitAccepted,
                )
                mutableNotice.value = if (physicalCommitAccepted) {
                    "Kitsu accepted enrollment; remote verification did not complete"
                } else {
                    "Enrollment did not reach physical confirmation"
                }
            } finally {
                enrollmentJob = null
            }
        }
    }

    fun stopGatewayEnrollmentBeforeConfirmation() {
        when (mutableGatewayEnrollmentState.value.stopDecision()) {
            GatewayEnrollmentStopDecision.STOP_AND_DISCONNECT -> disconnect(
                GatewayEnrollmentState.CancelledBeforePhysicalConfirmation,
                "Enrollment stop requested before acceptance was reported. Kitsu disconnected.",
            )
            GatewayEnrollmentStopDecision.KEEP_MONITORING_COMMITTED_BOOTSTRAP -> {
                mutableNotice.value =
                    "Kitsu already accepted enrollment. This app cannot cancel the device bootstrap."
            }
            GatewayEnrollmentStopDecision.NOTHING_TO_STOP -> Unit
        }
    }

    fun configureMesh(enabled: Boolean) = viewModelScope.launch {
        runCatching { repository.configureMesh(enabled) }
            .onSuccess { receipt ->
                mutableNotice.value = if (receipt.enabled) {
                    "Mesh enabled: UK/EU Narrow at ${receipt.txPowerDbm} dBm"
                } else {
                    "Mesh radio disabled"
                }
            }
            .onFailure { mutableNotice.value = "Mesh setting was not changed" }
    }

    fun refreshRemoteCompanions() = viewModelScope.launch {
        repository.refreshRemoteCompanions()
        refreshOwnerAccountStatus()
    }

    fun refreshGatewayCatalog() = viewModelScope.launch {
        repository.refreshGatewayCatalog()
    }

    fun connectMobileRelayDevice(deviceAddress: String) = viewModelScope.launch {
        runCatching {
            if (pairingJob?.isActive == true || deviceSetupInProgress()) {
                throw app.kitsu.mobile.transport.TransportException("mobile_relay_setup_busy")
            }
            val relayState = services.mobileRelayController.state.value
            if (relayState.teardownInProgress ||
                (!relayState.enabled && relayState.running)
            ) {
                throw app.kitsu.mobile.transport.TransportException(
                    "mobile_relay_teardown_in_progress",
                )
            }
            services.mobileRelayController.preflightConnectDevice(deviceAddress)
            if (!mobileRelayOwnsBluetooth()) {
                val pendingConnection = connectionJob
                pendingConnection?.cancel(CancellationException("mobile_relay_enabled"))
                pendingConnection?.join()
                repository.releaseDirectForMobileRelay(deviceAddress)
            }
            services.mobileRelayController.connectDevice(deviceAddress)
        }.onFailure { failure ->
            mutableNotice.value = when (
                (failure as? app.kitsu.mobile.transport.TransportException)?.code
            ) {
                "mobile_relay_pairing_required" -> "That Kitsu pairing is no longer available"
                "mobile_relay_setup_busy" ->
                    "Finish the current device setup before connecting the public gateway"
                "mobile_relay_teardown_in_progress" ->
                    "Wait for the public gateway to finish disconnecting"
                "bluetooth_permission_required" ->
                    "Allow Nearby devices in the Kitsu gateway card, then retry"
                "bluetooth_disabled" ->
                    "Turn on Bluetooth in the Kitsu gateway card, then retry"
                else -> "Public gateway connection was not started"
            }
        }
    }

    fun disconnectMobileRelayDevice(deviceAddress: String) = viewModelScope.launch {
        runCatching { services.mobileRelayController.disconnectDevice(deviceAddress) }
            .onFailure { mutableNotice.value = "That public gateway connection was not stopped" }
    }

    fun disconnectMobileRelayAll() = viewModelScope.launch {
        runCatching { services.mobileRelayController.disconnectAll() }
            .onFailure { mutableNotice.value = "Public gateway connections were not stopped" }
    }

    fun forgetMobileRelay() = viewModelScope.launch {
        runCatching {
            if (pairingJob?.isActive == true || deviceSetupInProgress()) {
                throw app.kitsu.mobile.transport.TransportException("mobile_relay_setup_busy")
            }
            val pendingConnection = connectionJob
            pendingConnection?.cancel(CancellationException("mobile_relay_forget"))
            pendingConnection?.join()
            repository.releaseDirectForMobileRelay()
            services.mobileRelayController.forgetRelay()
        }
            .onSuccess {
                mutableNotice.value = "Public gateway forgotten; phone pairings were kept"
            }
            .onFailure { failure ->
                mutableNotice.value = when (failure) {
                    is app.kitsu.mobile.relay.MobileRelayDeviceCleanupException -> when (
                        failure.cleanupCode
                    ) {
                        "mobile_relay_pairing_required",
                        "bond_missing_repair_required" ->
                            "Re-pair ${failure.deviceName} with this phone, then tap Finish forgetting"
                        else -> "Bring ${failure.deviceName} nearby, then tap Finish forgetting · " +
                            failure.cleanupCode.replace('_', ' ')
                    }
                    else -> "Public gateway was not forgotten · " +
                        ((failure as? app.kitsu.mobile.transport.TransportException)?.code
                            ?: "cleanup failed").replace('_', ' ')
                }
            }
    }

    fun selectRemoteCompanion(id: String) = viewModelScope.launch {
        runCatching { repository.selectRemoteCompanion(id) }
            .onFailure { mutableNotice.value = "Companion selection failed" }
        refreshOwnerAccountStatus()
    }

    fun pairController(label: String) {
        if (mobileRelayOwnsBluetooth()) {
            mutableNotice.value = "Disconnect the public gateway before pairing another Kitsu"
            return
        }
        if (pairingJob?.isActive == true) return
        if (connectionJob?.isActive == true || disconnectJob?.isActive == true) {
            mutableNotice.value = "Wait for the current connection change before pairing this phone"
            return
        }
        if (deviceSetupInProgress()) {
            mutableNotice.value = "Finish the current device setup step before pairing this phone"
            return
        }
        pairingJob = viewModelScope.launch {
            try {
                repository.pairController(label)
                services.mobileRelayController.refreshDevices()
                mutableNotice.value = "Kitsu paired securely"
            } catch (failure: Throwable) {
                val code = (failure as? app.kitsu.mobile.pairing.PairingException)?.code
                if (failure is kotlinx.coroutines.CancellationException || code == "pairing_cancelled") {
                    mutableNotice.value = "Pairing cancelled"
                } else {
                    mutableNotice.value = when (code) {
                        "bluetooth_permission_required" ->
                            "Bluetooth permission is required to pair this phone"
                        "bluetooth_disabled" -> "Turn on Bluetooth to pair this phone"
                        "bluetooth_unavailable" ->
                            "Bluetooth Low Energy is unavailable on this phone"
                        "location_services_disabled" ->
                            "Turn on Android Location services to scan on this Android version"
                        "controller_already_paired" ->
                            "This phone already has a saved Kitsu pairing"
                        "controller_device_limit" ->
                            "This phone already has the maximum of three saved Kitsu pairings"
                        "pairing_device_absent" ->
                            "No Kitsu in Pair Phone mode was found nearby"
                        "location_services_unavailable" ->
                            "Android Location services are unavailable on this phone"
                        "scanner_unavailable", "scan_start_failed" ->
                            "Android could not start the Bluetooth scan"
                        "secure_bond_failed" ->
                            "Android secure pairing failed; reopen Pair Phone on Kitsu and retry"
                        "gatt_timeout", "gatt_connect_failed", "gatt_disconnected",
                        "gatt_write_failed" ->
                            "The Bluetooth pairing connection ended; reopen Pair Phone and retry"
                        "pairing_timeout", "timeout" ->
                            "Pairing timed out; reopen Pair Phone on Kitsu and retry"
                        "pairing_closed" ->
                            "Kitsu closed Pair Phone mode; reopen it and retry"
                        "controller_full" ->
                            "Kitsu has no free phone slot"
                        "auth_failed" ->
                            "Kitsu rejected the secure pairing proof"
                        "credential_store_failed" ->
                            "Android could not securely save this Kitsu pairing"
                        "invalid_label" ->
                            "Use a 1–24 byte phone label without control characters"
                        else -> when {
                            code?.startsWith("scan_failed_") == true ->
                                "Android could not complete the Bluetooth scan"
                            code?.startsWith("gatt_status_") == true ->
                                "Android could not open the Bluetooth pairing connection"
                            else -> "Pairing failed"
                        }
                    }
                }
            } finally {
                pairingJob = null
            }
        }
    }

    fun cancelPairing() {
        repository.cancelPairing()
    }

    fun clearNotice() {
        mutableNotice.value = null
    }

    private fun perform(command: ActionCommand) = viewModelScope.launch {
        runCatching { repository.perform(command) }
            .onSuccess { mutableNotice.value = if (it.accepted) "Action accepted" else "Action rejected" }
            .onFailure { mutableNotice.value = "Action rejected" }
    }

    private fun requestId(): String = UUID.randomUUID().toString()

    private fun deviceSetupInProgress(): Boolean =
        provisioningJob?.isActive == true || enrollmentJob?.isActive == true

    private fun mobileRelayOwnsBluetooth(): Boolean =
        services.mobileRelayController.state.value.let {
            it.enabled || it.running || it.teardownInProgress || it.cleanupInProgress
        }

    private companion object {
        val WIFI_ACCEPTED_VERIFICATION_ERRORS = setOf(
            "wifi_storage_not_confirmed",
            "wifi_storage_verification_unavailable",
        )
    }
}

internal enum class GatewayEnrollmentStopDecision {
    STOP_AND_DISCONNECT,
    KEEP_MONITORING_COMMITTED_BOOTSTRAP,
    NOTHING_TO_STOP,
}

internal fun MainViewModel.GatewayEnrollmentState.stopDecision(): GatewayEnrollmentStopDecision =
    when (this) {
        MainViewModel.GatewayEnrollmentState.CreatingClaim,
        is MainViewModel.GatewayEnrollmentState.WaitingForPhysicalConfirmation ->
            GatewayEnrollmentStopDecision.STOP_AND_DISCONNECT
        MainViewModel.GatewayEnrollmentState.SwitchingToWifi,
        MainViewModel.GatewayEnrollmentState.PollingBackend ->
            GatewayEnrollmentStopDecision.KEEP_MONITORING_COMMITTED_BOOTSTRAP
        else -> GatewayEnrollmentStopDecision.NOTHING_TO_STOP
    }

internal fun MainViewModel.GatewayEnrollmentState.physicalCommitAccepted(): Boolean = when (this) {
    MainViewModel.GatewayEnrollmentState.SwitchingToWifi,
    MainViewModel.GatewayEnrollmentState.PollingBackend,
    MainViewModel.GatewayEnrollmentState.Complete,
    MainViewModel.GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit -> true
    is MainViewModel.GatewayEnrollmentState.Failed -> physicalCommitAccepted
    else -> false
}

internal fun MainViewModel.GatewayEnrollmentState.canStartNewEnrollment(): Boolean = when (this) {
    MainViewModel.GatewayEnrollmentState.Idle,
    MainViewModel.GatewayEnrollmentState.CancelledBeforePhysicalConfirmation -> true
    is MainViewModel.GatewayEnrollmentState.Failed -> !physicalCommitAccepted
    else -> false
}
