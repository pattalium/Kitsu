package app.kitsu.mobile.relay

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.security.BondedCompanion
import app.kitsu.mobile.security.CredentialStore
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.TransportException
import java.security.SecureRandom
import java.time.Instant
import java.util.Base64
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.Json

data class MobileRelayUiState(
    val enabled: Boolean = false,
    val running: Boolean = false,
    val pairedDeviceCount: Int = 0,
    val selectedDeviceCount: Int = 0,
    val detail: String = "off",
    val enrollmentRemainingMillis: Int? = null,
    val backendConnected: Boolean = false,
    val teardownInProgress: Boolean = false,
    val cleanupInProgress: Boolean = false,
    val cleanupPending: Boolean = false,
    val hasRelayConfiguration: Boolean = false,
    val devices: List<MobileRelayDeviceUiState> = emptyList(),
)

data class MobileRelayDeviceUiState(
    val deviceAddress: String,
    val displayName: String,
    val selected: Boolean = false,
    val bluetoothConnected: Boolean = false,
    val gatewayEnrolled: Boolean = false,
    val detail: String = "off",
    val enrollmentRemainingMillis: Int? = null,
    val cleanupRequired: Boolean = false,
    val cleanupComplete: Boolean = false,
)

class MobileRelayDeviceCleanupException(
    val deviceName: String,
    val cleanupCode: String,
    cause: Throwable? = null,
) : Exception(cleanupCode, cause)

/**
 * Owns the opt-in preference and the deliberately small foreground relay loop.
 * Enrollment documents and signed frames remain byte-exact and memory-only.
 */
class MobileRelayController(
    private val context: Context,
    private val credentials: CredentialStore,
    private val backend: MobileRelayBackend,
    private val sessions: MobileRelayDeviceSessionFactory,
    private val onSessionsClosed: suspend () -> Unit = {},
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val settingsMutex = Mutex()
    private val initialized = CompletableDeferred<Unit>()
    private val mutableState = MutableStateFlow(MobileRelayUiState())
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }
    private val random = SecureRandom()
    private val enrollmentAttemptRequests = ConcurrentHashMap.newKeySet<String>()
    private val nextSpoolRecordId = AtomicLong(System.currentTimeMillis().coerceAtLeast(1L))
    @Volatile private var foregroundCompletion = completedSignal()

    val state: StateFlow<MobileRelayUiState> = mutableState.asStateFlow()

    init {
        scope.launch {
            try {
                initialize()
            } finally {
                initialized.complete(Unit)
            }
        }
    }

    suspend fun awaitInitialized() = initialized.await()

    fun disableAfterForegroundStartFailure(code: String = "foreground_permission_required") {
        scope.launch {
            settingsMutex.withLock {
                val current = credentials.mobileRelaySettings() ?: return@withLock
                val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
                val updated = current.copy(
                    enabled = false,
                    activationAttempted = current.activationAttempted ||
                        current.selectedDeviceAddresses.isNotEmpty(),
                    selectedDeviceAddresses = emptyList(),
                )
                credentials.saveMobileRelaySettings(updated)
                enrollmentAttemptRequests.clear()
                publish(updated, bonds, running = false)
                mutableState.update {
                    it.copy(
                        enabled = false,
                        running = false,
                        teardownInProgress = false,
                        detail = code,
                    )
                }
            }
            runCatching { onSessionsClosed() }
            context.stopService(serviceIntent())
        }
    }

    suspend fun preflightConnectDevice(deviceAddress: String) = settingsMutex.withLock {
        val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
        if (bonds.none { it.deviceAddress.equals(deviceAddress, ignoreCase = true) }) {
            throw TransportException("mobile_relay_pairing_required")
        }
        bluetoothReadinessError()?.let { code ->
            publish(loadOrCreateSettings(), bonds)
            updateDevice(deviceAddress) {
                it.copy(
                    bluetoothConnected = false,
                    gatewayEnrolled = false,
                    detail = code,
                    enrollmentRemainingMillis = null,
                )
            }
            throw TransportException(code)
        }
    }

    suspend fun connectDevice(deviceAddress: String) = settingsMutex.withLock {
        val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
        val bond = bonds.singleOrNull {
            it.deviceAddress.equals(deviceAddress, ignoreCase = true)
        } ?: throw TransportException("mobile_relay_pairing_required")
        val current = loadOrCreateSettings()
        if (current.forgetPending) throw TransportException("public_gateway_cleanup_pending")
        if (mutableState.value.teardownInProgress ||
            (!current.enabled && mutableState.value.running)
        ) {
            throw TransportException("mobile_relay_teardown_in_progress")
        }
        bluetoothReadinessError()?.let { code ->
            publish(current, bonds)
            updateDevice(bond.deviceAddress) {
                it.copy(
                    bluetoothConnected = false,
                    gatewayEnrolled = false,
                    detail = code,
                    enrollmentRemainingMillis = null,
                )
            }
            throw TransportException(code)
        }
        val existingSelection = if (current.enabled || mutableState.value.running) {
            current.selectedDeviceAddresses
        } else {
            emptyList()
        }
        val selected = existingSelection.filterNot {
            it.equals(bond.deviceAddress, ignoreCase = true)
        }.plus(bond.deviceAddress).takeLast(MAX_MOBILE_RELAY_DEVICES)
        val updated = current.copy(
            enabled = true,
            activationAttempted = true,
            selectedDeviceAddresses = selected,
        )
        credentials.saveMobileRelaySettings(updated)
        enrollmentAttemptRequests += bond.deviceAddress.lowercase()
        publish(updated, bonds)
        updateDevice(bond.deviceAddress) {
            it.copy(selected = true, detail = "connecting_bluetooth")
        }
        startService()
    }

    suspend fun disconnectDevice(deviceAddress: String) {
        var stopAndRestart = false
        var restart = false
        settingsMutex.withLock {
            val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
            val current = loadOrCreateSettings()
            val targetSteady = mutableState.value.devices.singleOrNull {
                it.deviceAddress.equals(deviceAddress, ignoreCase = true)
            }?.gatewayEnrolled == true
            val selected = current.selectedDeviceAddresses.filterNot {
                it.equals(deviceAddress, ignoreCase = true)
            }
            val keepRunning = current.enabled && selected.isNotEmpty()
            stopAndRestart = !targetSteady || !keepRunning
            restart = stopAndRestart && keepRunning
            val updated = current.copy(
                enabled = keepRunning,
                activationAttempted = current.activationAttempted ||
                    current.selectedDeviceAddresses.isNotEmpty(),
                selectedDeviceAddresses = selected,
            )
            credentials.saveMobileRelaySettings(updated)
            enrollmentAttemptRequests -= deviceAddress.lowercase()
            updateDevice(deviceAddress) { it.copy(selected = false, detail = "disconnecting") }
            if (stopAndRestart) {
                mutableState.update {
                    it.copy(teardownInProgress = true, detail = "disconnecting")
                }
            }
            publish(updated, bonds)
        }
        if (!stopAndRestart) return
        stopForegroundRelayAndAwait()
        val shouldRestart = restart && settingsMutex.withLock {
            credentials.mobileRelaySettings()?.let {
                it.enabled && !it.forgetPending && it.selectedDeviceAddresses.isNotEmpty()
            } == true
        }
        if (shouldRestart) startService()
    }

    suspend fun requestDisconnectAll() {
        settingsMutex.withLock {
            val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
            val current = loadOrCreateSettings()
            val updated = current.copy(
                enabled = false,
                activationAttempted = current.activationAttempted ||
                    current.selectedDeviceAddresses.isNotEmpty(),
                selectedDeviceAddresses = emptyList(),
            )
            credentials.saveMobileRelaySettings(updated)
            enrollmentAttemptRequests.clear()
            mutableState.update { currentState ->
                currentState.copy(
                    enabled = false,
                    teardownInProgress = true,
                    detail = "disconnecting",
                    devices = currentState.devices.map {
                        if (it.selected || it.bluetoothConnected) {
                            it.copy(selected = false, detail = "disconnecting")
                        } else {
                            it
                        }
                    },
                )
            }
            publish(updated, bonds)
        }
        context.stopService(serviceIntent())
    }

    suspend fun disconnectAll() {
        requestDisconnectAll()
        stopForegroundRelayAndAwait()
    }

    suspend fun forgetRelay() {
        val (snapshot, bonds) = settingsMutex.withLock {
            val currentBonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
            val current = credentials.mobileRelaySettings()
                ?: throw TransportException("relay_credential_unavailable")
            validateSettings(current, currentBonds, allowMissingSelectedBonds = true)
            val pendingCleanup = current.copy(enabled = false, forgetPending = true)
            credentials.saveMobileRelaySettings(pendingCleanup)
            enrollmentAttemptRequests.clear()
            publish(pendingCleanup, currentBonds)
            mutableState.update {
                it.copy(
                    enabled = false,
                    cleanupInProgress = true,
                    cleanupPending = true,
                    detail = "forgetting_public_gateway",
                    enrollmentRemainingMillis = null,
                )
            }
            current to currentBonds
        }

        try {
            stopForegroundRelayAndAwait()
            backend.forgetRelay(snapshot.installationId)
            forgetGatewayOnDevices(snapshot, bonds)
            settingsMutex.withLock {
                val current = credentials.mobileRelaySettings()
                if (current?.installationId != snapshot.installationId) {
                    throw TransportException("mobile_relay_binding_changed")
                }
                credentials.saveMobileRelaySettings(null)
                mutableState.value = MobileRelayUiState(
                    pairedDeviceCount = bonds.size,
                    detail = "public_gateway_forgotten",
                    devices = bonds.map { bond ->
                        MobileRelayDeviceUiState(
                            deviceAddress = bond.deviceAddress,
                            displayName = bond.displayName,
                        )
                    },
                )
            }
        } catch (cancelled: CancellationException) {
            mutableState.update {
                it.copy(
                    running = false,
                    backendConnected = false,
                    cleanupInProgress = false,
                    cleanupPending = true,
                    detail = "public_gateway_cleanup_pending",
                )
            }
            throw cancelled
        } catch (failure: Throwable) {
            mutableState.update {
                it.copy(
                    running = false,
                    backendConnected = false,
                    cleanupInProgress = false,
                    cleanupPending = true,
                    detail = (failure as? MobileRelayDeviceCleanupException)?.cleanupCode
                        ?: (failure as? TransportException)?.code
                        ?: "public_gateway_forget_failed",
                )
            }
            throw failure
        }
    }

    private suspend fun forgetGatewayOnDevices(
        settings: MobileRelaySettings,
        bonds: List<BondedCompanion>,
    ) {
        val requiredAddresses = buildSet {
            settings.selectedDeviceAddresses.forEach { add(it.lowercase()) }
            settings.configuredDeviceAddresses.forEach { add(it.lowercase()) }
            settings.companionBindings.mapNotNull(MobileRelayCompanionBinding::deviceAddress)
                .forEach { add(it.lowercase()) }
        }
        val requiredHardwareUids = buildSet {
            settings.companionBindings.forEach { add(it.hardwareUid) }
            settings.pendingEnrollment?.hardwareUid?.let(::add)
        }
        val hasLegacyBinding = settings.companionBindings.any { it.deviceAddress == null } ||
            settings.pendingEnrollment != null
        if (hasLegacyBinding && bonds.isEmpty() && requiredHardwareUids.isNotEmpty()) {
            throw MobileRelayDeviceCleanupException(
                requiredHardwareUids.first(),
                "mobile_relay_pairing_required",
            )
        }
        val knownBondAddresses = bonds.mapTo(hashSetOf()) { it.deviceAddress.lowercase() }
        val missingAddress = requiredAddresses.firstOrNull { it !in knownBondAddresses }
        if (missingAddress != null) {
            throw MobileRelayDeviceCleanupException(
                missingAddress,
                "mobile_relay_pairing_required",
            )
        }
        val candidates = if (hasLegacyBinding) {
            bonds
        } else {
            bonds.filter { it.deviceAddress.lowercase() in requiredAddresses }
        }
        val gatewayId = MobileRelayWirePolicy.gatewayId(settings.installationId)
        for (bond in candidates) {
            val address = bond.deviceAddress.lowercase()
            val session = sessions.create(bond)
            var required = address in requiredAddresses
            try {
                updateDevice(address) {
                    it.copy(
                        detail = "forgetting_public_gateway",
                        enrollmentRemainingMillis = null,
                        cleanupRequired = true,
                        cleanupComplete = false,
                    )
                }
                when (val connected = session.connect()) {
                    ConnectResult.Connected -> Unit
                    ConnectResult.CompanionAbsent -> throw TransportException("companion_absent")
                    is ConnectResult.PermissionRequired ->
                        throw TransportException("bluetooth_permission_required")
                    is ConnectResult.Failed -> throw TransportException(connected.code)
                }
                val status = session.status()
                required = required || status.deviceId in requiredHardwareUids
                if (!required) {
                    updateDevice(address) {
                        it.copy(
                            detail = "public_gateway_cleanup_not_needed",
                            cleanupRequired = false,
                            cleanupComplete = false,
                        )
                    }
                    continue
                }
                val receipt = session.forgetGateway(gatewayId)
                verifyForgetReceipt(receipt)
                updateDevice(address) {
                    it.copy(
                        selected = false,
                        bluetoothConnected = false,
                        gatewayEnrolled = false,
                        detail = "public_gateway_forgotten",
                        enrollmentRemainingMillis = null,
                        cleanupRequired = true,
                        cleanupComplete = true,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                val code = (failure as? TransportException)?.code
                    ?: "public_gateway_forget_failed"
                updateDevice(address) {
                    it.copy(
                        bluetoothConnected = false,
                        detail = code,
                        enrollmentRemainingMillis = null,
                        cleanupRequired = true,
                        cleanupComplete = false,
                    )
                }
                throw MobileRelayDeviceCleanupException(bond.displayName, code, failure)
            } finally {
                runCatching { session.disconnect() }
            }
        }
    }

    suspend fun refreshDevices() = settingsMutex.withLock {
        val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
        val current = loadOrCreateSettings()
        val selected = current.selectedDeviceAddresses.filter { address ->
            bonds.any { it.deviceAddress.equals(address, ignoreCase = true) }
        }
        val updated = current.copy(
            enabled = current.enabled && selected.isNotEmpty(),
            activationAttempted = current.activationAttempted ||
                current.selectedDeviceAddresses.isNotEmpty(),
            selectedDeviceAddresses = selected,
        )
        if (updated != current) credentials.saveMobileRelaySettings(updated)
        publish(updated, bonds)
    }

    suspend fun runForegroundService() {
        initialized.await()
        if (enabledSettings() == null) return
        val completion = CompletableDeferred<Unit>()
        foregroundCompletion = completion
        var enrollmentPaused = false
        mutableState.update {
            it.copy(
                running = true,
                detail = "connecting_public_gateway",
                enrollmentRemainingMillis = null,
            )
        }
        try {
            while (kotlin.coroutines.coroutineContext.isActive) {
                val settings = enabledSettings() ?: break
                try {
                    val gatewayId = MobileRelayWirePolicy.gatewayId(settings.installationId)
                    val identity = backend.ensureRelay(settings.installationId, gatewayId)
                    runBoundRelay(settings, identity)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Throwable) {
                    SafeLog.warn("mobile_relay", "relay_retry", failure)
                    val code = (failure as? TransportException)?.code ?: "retrying"
                    if (code == "rate_limited" && !settings.activationComplete) {
                        pauseEnrollmentAttempt(settings.installationId, code)
                        enrollmentPaused = true
                        break
                    }
                    mutableState.update {
                        it.copy(
                            running = true,
                            detail = code,
                            enrollmentRemainingMillis = null,
                        )
                    }
                    delay(if (code == "rate_limited") RATE_LIMIT_RETRY_MILLIS else RETRY_MILLIS)
                }
            }
        } finally {
            enrollmentAttemptRequests.clear()
            runCatching { onSessionsClosed() }
            mutableState.update { currentState ->
                currentState.copy(
                    running = false,
                    backendConnected = false,
                    teardownInProgress = false,
                    selectedDeviceCount = if (currentState.enabled ||
                        currentState.cleanupInProgress
                    ) {
                        currentState.selectedDeviceCount
                    } else {
                        0
                    },
                    detail = if (enrollmentPaused || currentState.cleanupInProgress) {
                        currentState.detail
                    } else {
                        "off"
                    },
                    enrollmentRemainingMillis = null,
                    devices = currentState.devices.map {
                        val remainsSelected = currentState.cleanupInProgress ||
                            (currentState.enabled && it.selected)
                        it.copy(
                            selected = remainsSelected,
                            bluetoothConnected = false,
                            detail = if (remainsSelected) it.detail else "off",
                            enrollmentRemainingMillis = null,
                        )
                    },
                )
            }
            completion.complete(Unit)
        }
    }

    private suspend fun initialize() = settingsMutex.withLock {
        try {
            val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
            val loaded = loadOrCreateSettings()
            val settings = if (loaded.enabled &&
                (!loaded.activationComplete || loaded.forgetPending)
            ) {
                loaded.copy(enabled = false).also {
                    credentials.saveMobileRelaySettings(it)
                }
            } else {
                loaded
            }
            validateSettings(
                settings,
                bonds,
                allowMissingSelectedBonds = settings.forgetPending,
            )
            publish(settings, bonds, running = false)
            if (settings.forgetPending) {
                mutableState.update { it.copy(detail = "public_gateway_cleanup_pending") }
            }
            if (MobileRelaySettingsPolicy.canStartAutomatically(settings)) startService()
        } catch (failure: Throwable) {
            SafeLog.warn("mobile_relay", "relay_state_unavailable", failure)
            mutableState.value = MobileRelayUiState(detail = "secure_state_unavailable")
        }
    }

    private suspend fun runBoundRelay(
        initialSettings: MobileRelaySettings,
        identity: MobileRelayIdentity,
    ) = coroutineScope {
        val inbound = Channel<ByteArray>(capacity = MAX_PENDING_DOWNLINKS)
        val pending = mutableListOf<PendingDownlink>()
        val deliveredActionIds = linkedSetOf<String>()
        val retainedSessions = mutableMapOf<String, MobileRelayDeviceSession>()
        val connectedSessions = mutableSetOf<String>()
        val configuredSessions = mutableSetOf<String>()
        val companionIds = mutableMapOf<String, String>()
        val pausedEnrollments = mutableMapOf<String, String>()
        val uplinkRetryAfterNanos = mutableMapOf<String, Long>()
        val websocket = launch {
            while (isActive) {
                try {
                    val settings = enabledSettings() ?: break
                    if (settings.installationId != initialSettings.installationId) break
                    if (!settings.activationComplete) {
                        updateBackendState(
                            MobileRelayBackendConnectionState(detail = "waiting_for_activation"),
                        )
                        delay(RETRY_MILLIS)
                        continue
                    }
                    backend.downlinks(
                        initialSettings.installationId,
                        ::updateBackendState,
                    ).collect(inbound::send)
                    delay(RETRY_MILLIS)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Throwable) {
                    SafeLog.warn("mobile_relay", "downlink_retry", failure)
                    val code = (failure as? TransportException)?.code
                    updateBackendState(
                        MobileRelayBackendConnectionState(detail = code ?: "device_relay_session_failed"),
                    )
                    delay(if (code == "rate_limited") RATE_LIMIT_RETRY_MILLIS else RETRY_MILLIS)
                }
            }
        }
        try {
            while (isActive) {
                val settings = enabledSettings() ?: return@coroutineScope
                if (settings.installationId != initialSettings.installationId) return@coroutineScope
                drainDownlinks(inbound, pending, deliveredActionIds)
                val bonds = selectedBonds(settings)
                if (bonds.isEmpty()) throw TransportException("mobile_relay_pairing_required")
                val selectedAddresses = bonds.mapTo(linkedSetOf()) {
                    it.deviceAddress.lowercase()
                }
                retainedSessions.keys.filterNot(selectedAddresses::contains).forEach { address ->
                    connectedSessions -= address
                    configuredSessions -= address
                    companionIds -= address
                    pausedEnrollments -= address
                    uplinkRetryAfterNanos -= address
                    retainedSessions.remove(address)?.let { retired ->
                        runCatching { retired.disconnect() }
                    }
                    updateDevice(address) {
                        it.copy(
                            selected = false,
                            bluetoothConnected = false,
                            gatewayEnrolled = false,
                            detail = "off",
                            enrollmentRemainingMillis = null,
                        )
                    }
                }
                pausedEnrollments.keys.filter { it in enrollmentAttemptRequests }.forEach {
                    pausedEnrollments -= it
                }
                var completed = 0
                var lastFailureCode: String? = null
                val enrollmentCandidates = mutableListOf<EnrollmentCandidate>()

                // Discover every selected device first. Existing bound devices are then
                // serviced before one bounded PRG enrollment can hold this sequential loop.
                val orderedBonds = bonds.sortedBy { bond ->
                    val address = bond.deviceAddress.lowercase()
                    if (address in companionIds || settings.companionBindings.any {
                            it.deviceAddress?.equals(address, ignoreCase = true) == true
                        }
                    ) 0 else 1
                }
                for (bond in orderedBonds) {
                    val address = bond.deviceAddress.lowercase()
                    val session = retainedSessions.getOrPut(address) {
                        sessions.create(bond)
                    }
                    try {
                        if (address !in connectedSessions || !session.isConnected()) {
                            connectedSessions -= address
                            configuredSessions -= address
                            companionIds -= address
                            updateDevice(address) {
                                it.copy(
                                    selected = true,
                                    bluetoothConnected = false,
                                    gatewayEnrolled = false,
                                    detail = "connecting_bluetooth",
                                    enrollmentRemainingMillis = null,
                                )
                            }
                            when (val connected = session.connect()) {
                                ConnectResult.Connected -> {
                                    connectedSessions += address
                                    updateDevice(address) {
                                        it.copy(
                                            selected = true,
                                            bluetoothConnected = true,
                                            detail = "configuring_public_gateway",
                                        )
                                    }
                                }
                                ConnectResult.CompanionAbsent ->
                                    throw TransportException("companion_absent")
                                is ConnectResult.PermissionRequired ->
                                    throw TransportException("bluetooth_permission_required")
                                is ConnectResult.Failed -> throw TransportException(connected.code)
                            }
                        }
                        if (address !in configuredSessions) {
                            verifyConfigureReceipt(
                                session.configureRelay(
                                    identity.gatewayId,
                                    identity.caCertificateDerB64,
                                ),
                            )
                            markDeviceConfigured(settings.installationId, bond.deviceAddress)
                            configuredSessions += address
                        }
                        if (address in companionIds) continue
                        pausedEnrollments[address]?.let { code ->
                            updateDevice(address) {
                                it.copy(
                                    selected = true,
                                    bluetoothConnected = session.isConnected(),
                                    gatewayEnrolled = false,
                                    detail = code,
                                )
                            }
                            return@let
                        }
                        if (address in pausedEnrollments) continue

                        // Configuration may invalidate credentials bound to a different gateway.
                        val status = session.status()
                        if (status.lan.gatewayEnrolled == true) {
                            val binding = settings.companionBindings
                                .singleOrNull { it.hardwareUid == status.deviceId }
                                ?: throw TransportException("mobile_relay_companion_binding_missing")
                            if (!binding.deviceAddress.equals(address, ignoreCase = true)) {
                                saveCompanionBinding(
                                    settings.installationId,
                                    status.deviceId,
                                    binding.companionId,
                                    bond.deviceAddress,
                                )
                            }
                            clearPendingEnrollment(settings.installationId, status.deviceId)
                            companionIds[address] = binding.companionId
                            markActivationComplete(settings.installationId)
                            updateDevice(address) {
                                it.copy(
                                    selected = true,
                                    bluetoothConnected = true,
                                    gatewayEnrolled = true,
                                    detail = "connected_public_gateway",
                                )
                            }
                        } else {
                            if (address !in enrollmentAttemptRequests) {
                                pausedEnrollments[address] = "connect_to_enroll"
                                updateDevice(address) {
                                    it.copy(
                                        selected = true,
                                        bluetoothConnected = true,
                                        gatewayEnrolled = false,
                                        detail = "connect_to_enroll",
                                    )
                                }
                            } else {
                                enrollmentCandidates += EnrollmentCandidate(
                                    bond,
                                    status.deviceId,
                                    status.displayName,
                                    session,
                                )
                            }
                        }
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        SafeLog.warn("mobile_relay", "device_retry", failure)
                        val code = (failure as? TransportException)?.code ?: "retrying_devices"
                        if (!session.isConnected()) {
                            connectedSessions -= address
                            configuredSessions -= address
                            companionIds -= address
                        }
                        updateDevice(address) {
                            it.copy(
                                selected = true,
                                bluetoothConnected = session.isConnected(),
                                gatewayEnrolled = address in companionIds,
                                detail = code,
                                enrollmentRemainingMillis = null,
                            )
                        }
                        if (code == "rate_limited") {
                            lastFailureCode = code
                        }
                        if (lastFailureCode != "existing_gateway_enrollment_requires_reset") {
                            lastFailureCode = code
                        }
                    }
                }

                // Steady devices always get their bounded relay pass before enrollment.
                for (bond in orderedBonds) {
                    val address = bond.deviceAddress.lowercase()
                    val companionId = companionIds[address] ?: continue
                    val session = retainedSessions[address] ?: continue
                    try {
                        if (System.nanoTime() >= (uplinkRetryAfterNanos[address] ?: 0L)) {
                            relayUplink(session, initialSettings.installationId)
                        }
                        deliverDownlinks(session, companionId, pending, deliveredActionIds)
                        completed += 1
                        updateDevice(address) {
                            it.copy(
                                selected = true,
                                bluetoothConnected = session.isConnected(),
                                gatewayEnrolled = true,
                                detail = "connected_public_gateway",
                            )
                        }
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        val code = (failure as? TransportException)?.code ?: "retrying_devices"
                        SafeLog.warn("mobile_relay", "device_retry", failure)
                        if (!session.isConnected()) {
                            connectedSessions -= address
                            configuredSessions -= address
                            companionIds -= address
                        }
                        updateDevice(address) {
                            it.copy(
                                bluetoothConnected = session.isConnected(),
                                gatewayEnrolled = address in companionIds,
                                detail = code,
                            )
                        }
                        if (code == "rate_limited") {
                            uplinkRetryAfterNanos[address] = System.nanoTime() +
                                RATE_LIMIT_RETRY_MILLIS * 1_000_000L
                        }
                        lastFailureCode = code
                    }
                }

                for (candidate in enrollmentCandidates) {
                        val address = candidate.bond.deviceAddress.lowercase()
                        if (!enrollmentAttemptRequests.remove(address)) continue
                        try {
                            val latestSettings = enabledSettings() ?: break
                            val companionId = enroll(
                                candidate.session,
                                address,
                                candidate.hardwareUid,
                                candidate.displayName,
                                latestSettings,
                                identity,
                            )
                            companionIds[address] = companionId
                            saveCompanionBinding(
                                settings.installationId,
                                candidate.hardwareUid,
                                companionId,
                                candidate.bond.deviceAddress,
                            )
                            markActivationComplete(initialSettings.installationId)
                            relayUplink(candidate.session, initialSettings.installationId)
                            deliverDownlinks(
                                candidate.session,
                                companionId,
                                pending,
                                deliveredActionIds,
                            )
                            completed += 1
                            updateDevice(address) {
                                it.copy(
                                    selected = true,
                                    bluetoothConnected = candidate.session.isConnected(),
                                    gatewayEnrolled = true,
                                    detail = "connected_public_gateway",
                                    enrollmentRemainingMillis = null,
                                )
                            }
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Throwable) {
                            val code = (failure as? TransportException)?.code
                                ?: "mobile_relay_enrollment_failed"
                            SafeLog.warn("mobile_relay", "enrollment_paused", failure)
                            pausedEnrollments[address] = code
                            updateDevice(address) {
                                it.copy(
                                    selected = true,
                                    bluetoothConnected = candidate.session.isConnected(),
                                    gatewayEnrolled = false,
                                    detail = code,
                                    enrollmentRemainingMillis = null,
                                )
                            }
                            lastFailureCode = code
                        }
                    }
                mutableState.update { currentState ->
                    currentState.copy(
                        running = true,
                        detail = if (completed == bonds.size && currentState.backendConnected) {
                            "connected_public_gateway"
                        } else {
                            enrollmentCandidates.firstNotNullOfOrNull { candidate ->
                                currentState.devices.singleOrNull {
                                    it.deviceAddress.equals(
                                        candidate.bond.deviceAddress,
                                        ignoreCase = true,
                                    )
                                }?.detail?.takeIf { it != "connected_public_gateway" }
                            } ?: lastFailureCode ?: if (completed > 0) {
                                "connecting_public_gateway"
                            } else {
                                "retrying_devices"
                            }
                        },
                        enrollmentRemainingMillis = null,
                    )
                }
                delay(DEVICE_POLL_MILLIS)
            }
        } finally {
            websocket.cancel()
            updateBackendState(MobileRelayBackendConnectionState())
            inbound.close()
            pending.forEach { it.exactFrame.fill(0) }
            retainedSessions.forEach { (address, session) ->
                runCatching { session.disconnect() }
                updateDevice(address) {
                    it.copy(
                        bluetoothConnected = false,
                        gatewayEnrolled = false,
                        detail = if (it.selected) "disconnected" else "off",
                        enrollmentRemainingMillis = null,
                    )
                }
            }
            connectedSessions.clear()
            configuredSessions.clear()
            companionIds.clear()
            pausedEnrollments.clear()
            uplinkRetryAfterNanos.clear()
            retainedSessions.clear()
        }
    }

    private suspend fun enroll(
        session: MobileRelayDeviceSession,
        deviceAddress: String,
        hardwareUid: String,
        displayName: String,
        settings: MobileRelaySettings,
        identity: MobileRelayIdentity,
    ): String {
        settings.pendingEnrollment?.let { pending ->
            if (pending.hardwareUid != hardwareUid) {
                throw TransportException("mobile_relay_enrollment_in_progress")
            }
            if (!Instant.parse(pending.expiresAt).isAfter(Instant.now())) {
                clearPendingEnrollment(settings.installationId, hardwareUid, pending.enrollmentId)
            } else {
                updateEnrollmentState(deviceAddress, "finishing_public_gateway")
                try {
                    return completeEnrollment(
                        session,
                        hardwareUid,
                        settings,
                        identity,
                        pending.enrollmentId,
                        recovering = true,
                    )
                } catch (failure: TransportException) {
                    if (failure.code !in STALE_ENROLLMENT_CODES) throw failure
                    // The explicit Connect action may discard one unusable local
                    // recovery record before creating its single fresh challenge.
                }
            }
        }
        val challenge = backend.createEnrollment(settings.installationId, hardwareUid, displayName)
        val enrollmentId = challenge.enrollment.id
        val begin = session.beginEnrollment(enrollmentId, challenge.claimToken)
        validateEnrollmentReceipt(begin, enrollmentId)
        val durationMillis = (begin.expiresInMs ?: DEFAULT_ENROLLMENT_WINDOW_MILLIS)
            .coerceIn(1_000, MAX_ENROLLMENT_WINDOW_MILLIS)
        val deadline = System.nanoTime() + durationMillis * 1_000_000L
        var finish: GatewayEnrollmentReceipt? = null
        while (System.nanoTime() < deadline) {
            val remainingMillis = ((deadline - System.nanoTime()) / 1_000_000L)
                .coerceIn(1L, Int.MAX_VALUE.toLong())
                .toInt()
            updateEnrollmentState(deviceAddress, "hold_prg_to_connect", remainingMillis)
            delay(ENROLLMENT_POLL_MILLIS)
            finish = session.finishEnrollment(enrollmentId)
            validateEnrollmentReceipt(finish, enrollmentId, allowPending = true)
            if (finish.accepted && finish.state == "ready_for_wifi") break
        }
        if (finish?.accepted != true || finish.state != "ready_for_wifi") {
            throw TransportException("mobile_relay_enrollment_expired")
        }
        updateEnrollmentState(deviceAddress, "finishing_public_gateway")
        savePendingEnrollment(
            settings.installationId,
            MobileRelayPendingEnrollment(
                hardwareUid = hardwareUid,
                enrollmentId = enrollmentId,
                expiresAt = challenge.enrollment.expiresAt,
            ),
        )
        return completeEnrollment(session, hardwareUid, settings, identity, enrollmentId)
    }

    private suspend fun completeEnrollment(
        session: MobileRelayDeviceSession,
        hardwareUid: String,
        settings: MobileRelaySettings,
        identity: MobileRelayIdentity,
        enrollmentId: String,
        recovering: Boolean = false,
    ): String {
        val exactRequest = try {
            MobileRelayTransfer.pull(MobileRelayPullKind.ENROLLMENT, session::pull)
        } catch (failure: TransportException) {
            if (recovering &&
                (failure.code == "enrollment_unavailable" ||
                    failure.code == "physical_confirmation_required")
            ) {
                clearPendingEnrollment(settings.installationId, hardwareUid, enrollmentId)
            }
            throw failure
        }
        if (exactRequest == null) {
            if (recovering) {
                clearPendingEnrollment(settings.installationId, hardwareUid, enrollmentId)
            }
            throw TransportException("mobile_relay_enrollment_request_missing")
        }
        var exactResponse: ByteArray? = null
        try {
            exactResponse = backend.claimEnrollment(
                settings.installationId,
                enrollmentId,
                exactRequest,
            )
            val response = runCatching {
                json.decodeFromString(
                    MobileRelayClaimResponse.serializer(),
                    exactResponse.toString(Charsets.UTF_8),
                )
            }.getOrElse { throw TransportException("malformed_enrollment_response", it) }
            if (response.gatewayId != identity.gatewayId ||
                !MobileRelayWirePolicy.canonicalUuid(response.companionId)
            ) throw TransportException("mobile_relay_enrollment_binding_failed")
            saveCompanionBinding(settings.installationId, hardwareUid, response.companionId)
            MobileRelayTransfer.push(MobileRelayPushKind.ENROLLMENT, exactResponse, session::push)
            clearPendingEnrollment(settings.installationId, hardwareUid, enrollmentId)
            return response.companionId
        } finally {
            exactRequest.fill(0)
            exactResponse?.fill(0)
        }
    }

    private suspend fun relayUplink(session: MobileRelayDeviceSession, installationId: String) {
        repeat(MAX_UPLINKS_PER_DEVICE_CYCLE) {
            val exactEnvelope = MobileRelayTransfer.pull(MobileRelayPullKind.UPLINK, session::pull)
                ?: return
            var exactAcknowledgement: ByteArray? = null
            try {
                val spoolRecordId = nextSpoolRecordId.updateAndGet { current ->
                    if (current == Long.MAX_VALUE) 1L else current + 1L
                }.toString()
                exactAcknowledgement = backend.uploadEnvelope(
                    installationId,
                    spoolRecordId,
                    exactEnvelope,
                )
                MobileRelayTransfer.push(
                    MobileRelayPushKind.DOWNLINK,
                    exactAcknowledgement,
                    session::push,
                )
            } finally {
                exactEnvelope.fill(0)
                exactAcknowledgement?.fill(0)
            }
        }
    }

    private suspend fun deliverDownlinks(
        session: MobileRelayDeviceSession,
        companionId: String,
        pending: MutableList<PendingDownlink>,
        deliveredActionIds: MutableSet<String>,
    ) {
        var index = 0
        while (index < pending.size) {
            val item = pending[index]
            if (item.companionId != companionId) {
                index += 1
                continue
            }
            MobileRelayTransfer.push(MobileRelayPushKind.DOWNLINK, item.exactFrame, session::push)
            rememberDelivered(item.actionId, deliveredActionIds)
            item.exactFrame.fill(0)
            pending.removeAt(index)
        }
    }

    private fun drainDownlinks(
        inbound: Channel<ByteArray>,
        pending: MutableList<PendingDownlink>,
        deliveredActionIds: Set<String>,
    ) {
        while (pending.size < MAX_PENDING_DOWNLINKS) {
            val result = inbound.tryReceive()
            val frame = result.getOrNull() ?: break
            val header = runCatching {
                json.decodeFromString(
                    MobileRelayRemoteActionHeader.serializer(),
                    frame.toString(Charsets.UTF_8),
                )
            }.getOrNull()
            if (header == null || header.schema != MobileRelayWirePolicy.REMOTE_ACTION_SCHEMA ||
                !MobileRelayWirePolicy.canonicalUuid(header.actionId) ||
                !MobileRelayWirePolicy.canonicalUuid(header.companionId) ||
                header.actionId in deliveredActionIds || pending.any { it.actionId == header.actionId }
            ) {
                frame.fill(0)
                continue
            }
            pending += PendingDownlink(header.actionId, header.companionId, frame)
        }
    }

    private fun rememberDelivered(actionId: String, deliveredActionIds: MutableSet<String>) {
        deliveredActionIds += actionId
        while (deliveredActionIds.size > MAX_DELIVERED_ACTION_IDS) {
            val oldest = deliveredActionIds.firstOrNull() ?: break
            deliveredActionIds -= oldest
        }
    }

    private suspend fun selectedBonds(settings: MobileRelaySettings): List<BondedCompanion> {
        val selected = settings.selectedDeviceAddresses
        return credentials.bondedCompanions()
            .filter { bond -> selected.any { it.equals(bond.deviceAddress, ignoreCase = true) } }
            .distinctBy { it.controllerIdB64 }
            .take(MAX_MOBILE_RELAY_DEVICES)
    }

    private suspend fun enabledSettings(): MobileRelaySettings? {
        val settings = credentials.mobileRelaySettings() ?: return null
        if (!settings.enabled) return null
        validateSettings(settings, credentials.bondedCompanions())
        return settings
    }

    private suspend fun loadOrCreateSettings(): MobileRelaySettings {
        val existing = credentials.mobileRelaySettings()
        if (existing != null &&
            MobileRelayWirePolicy.canonicalRelayCredential(existing.relayCredentialB64)
        ) return existing
        // 1.1.0 used an owner-scoped relay with the same deterministic gateway
        // derivation. Rotate both identifiers so the first account-free connect
        // deliberately reconfigures and re-enrolls instead of colliding with it.
        return MobileRelaySettingsPolicy.migrateLegacy(
            existing = existing,
            installationId = UUID.randomUUID().toString(),
            relayCredentialB64 = newRelayCredential(),
        ).also { credentials.saveMobileRelaySettings(it) }
    }

    private fun validateSettings(
        settings: MobileRelaySettings,
        bonds: List<BondedCompanion>,
        allowMissingSelectedBonds: Boolean = false,
    ) {
        if (!MobileRelayWirePolicy.canonicalUuid(settings.installationId) ||
            !MobileRelayWirePolicy.canonicalRelayCredential(settings.relayCredentialB64) ||
            settings.selectedDeviceAddresses.size > MAX_MOBILE_RELAY_DEVICES ||
            settings.selectedDeviceAddresses.distinctBy(String::lowercase).size !=
            settings.selectedDeviceAddresses.size ||
            (!allowMissingSelectedBonds && settings.selectedDeviceAddresses.any { selected ->
                bonds.none { it.deviceAddress.equals(selected, ignoreCase = true) }
            }) || settings.configuredDeviceAddresses.size > MAX_MOBILE_RELAY_DEVICES ||
            settings.configuredDeviceAddresses.distinctBy(String::lowercase).size !=
            settings.configuredDeviceAddresses.size ||
            settings.configuredDeviceAddresses.any(String::isBlank) ||
            settings.companionBindings.size > MAX_MOBILE_RELAY_DEVICES ||
            settings.companionBindings.distinctBy(MobileRelayCompanionBinding::hardwareUid).size !=
            settings.companionBindings.size ||
            settings.companionBindings.distinctBy(MobileRelayCompanionBinding::companionId).size !=
            settings.companionBindings.size ||
            settings.companionBindings.mapNotNull(MobileRelayCompanionBinding::deviceAddress)
                .distinctBy(String::lowercase).size !=
            settings.companionBindings.count { it.deviceAddress != null } ||
            settings.companionBindings.any {
                !HARDWARE_UID.matches(it.hardwareUid) ||
                    !MobileRelayWirePolicy.canonicalUuid(it.companionId) ||
                    it.deviceAddress?.isBlank() == true
            } || settings.pendingEnrollment?.let {
                !HARDWARE_UID.matches(it.hardwareUid) ||
                    !MobileRelayWirePolicy.canonicalUuid(it.enrollmentId) ||
                    runCatching { Instant.parse(it.expiresAt) }.getOrNull() == null
            } == true || (settings.enabled && settings.selectedDeviceAddresses.isEmpty())
        ) throw TransportException("invalid_mobile_relay_settings")
    }

    private fun publish(
        settings: MobileRelaySettings,
        bonds: List<BondedCompanion>,
        running: Boolean? = null,
    ) {
        mutableState.update { previous ->
            val isRunning = running ?: previous.running
            val previousDevices = previous.devices.associateBy { it.deviceAddress.lowercase() }
            val knownCleanupAddresses = buildSet {
                settings.selectedDeviceAddresses.forEach { add(it.lowercase()) }
                settings.configuredDeviceAddresses.forEach { add(it.lowercase()) }
                settings.companionBindings.mapNotNull(MobileRelayCompanionBinding::deviceAddress)
                    .forEach { add(it.lowercase()) }
            }
            val probeLegacyCleanup = settings.companionBindings.any { it.deviceAddress == null } ||
                settings.pendingEnrollment != null
            val devices = bonds.take(MAX_MOBILE_RELAY_DEVICES).map { bond ->
                val selected = settings.selectedDeviceAddresses.any {
                    it.equals(bond.deviceAddress, ignoreCase = true)
                }
                val old = previousDevices[bond.deviceAddress.lowercase()]
                val cleanupRequired = settings.forgetPending &&
                    (probeLegacyCleanup || bond.deviceAddress.lowercase() in knownCleanupAddresses)
                val cleanupComplete = cleanupRequired && old?.cleanupComplete == true
                MobileRelayDeviceUiState(
                    deviceAddress = bond.deviceAddress,
                    displayName = bond.displayName,
                    selected = selected,
                    bluetoothConnected = old?.bluetoothConnected == true && isRunning,
                    gatewayEnrolled = old?.gatewayEnrolled == true && isRunning,
                    detail = when {
                        cleanupComplete -> "public_gateway_forgotten"
                        cleanupRequired && old?.cleanupRequired == true -> old.detail
                        cleanupRequired -> "waiting_public_gateway_cleanup"
                        settings.forgetPending -> "public_gateway_cleanup_not_needed"
                        old == null -> if (selected) "connecting_bluetooth" else "off"
                        !selected && old.bluetoothConnected -> "disconnecting"
                        !selected -> "off"
                        else -> old.detail
                    },
                    enrollmentRemainingMillis = old?.enrollmentRemainingMillis
                        ?.takeIf { isRunning },
                    cleanupRequired = cleanupRequired,
                    cleanupComplete = cleanupComplete,
                )
            }
            MobileRelayUiState(
                enabled = settings.enabled,
                running = isRunning,
                pairedDeviceCount = bonds.size.coerceAtMost(MAX_MOBILE_RELAY_DEVICES),
                selectedDeviceCount = settings.selectedDeviceAddresses.size,
                detail = if (settings.enabled) {
                    if (isRunning) previous.detail else "connecting_public_gateway"
                } else if (settings.selectedDeviceAddresses.isNotEmpty() &&
                    previous.detail != "off"
                ) {
                    previous.detail
                } else {
                    "off"
                },
                enrollmentRemainingMillis = if (isRunning) {
                    previous.enrollmentRemainingMillis
                } else {
                    null
                },
                backendConnected = previous.backendConnected && isRunning,
                teardownInProgress = previous.teardownInProgress,
                cleanupInProgress = previous.cleanupInProgress,
                cleanupPending = settings.forgetPending,
                hasRelayConfiguration = settings.hasRelayConfiguration(),
                devices = devices,
            )
        }
    }

    private fun MobileRelaySettings.hasRelayConfiguration(): Boolean =
        activationAttempted || activationComplete || forgetPending || pendingEnrollment != null ||
            selectedDeviceAddresses.isNotEmpty() || configuredDeviceAddresses.isNotEmpty() ||
            companionBindings.isNotEmpty()

    private fun updateDevice(
        deviceAddress: String,
        update: (MobileRelayDeviceUiState) -> MobileRelayDeviceUiState,
    ) {
        mutableState.update { current ->
            var changed = false
            val devices = current.devices.map { device ->
                if (device.deviceAddress.equals(deviceAddress, ignoreCase = true)) {
                    changed = true
                    val proposed = update(device)
                    if (!device.selected && proposed.selected) device else proposed
                } else {
                    device
                }
            }
            if (changed) current.copy(devices = devices) else current
        }
    }

    private fun updateEnrollmentState(
        deviceAddress: String,
        detail: String,
        remainingMillis: Int? = null,
    ) {
        updateDevice(deviceAddress) {
            it.copy(
                selected = true,
                bluetoothConnected = true,
                gatewayEnrolled = false,
                detail = detail,
                enrollmentRemainingMillis = remainingMillis,
            )
        }
        mutableState.update {
            it.copy(
                running = true,
                detail = detail,
                enrollmentRemainingMillis = remainingMillis,
            )
        }
    }

    private fun updateBackendState(connection: MobileRelayBackendConnectionState) {
        mutableState.update { current ->
            val allSelectedReady = current.devices.filter(MobileRelayDeviceUiState::selected)
                .let { it.isNotEmpty() && it.all(MobileRelayDeviceUiState::gatewayEnrolled) }
            current.copy(
                backendConnected = connection.connected,
                detail = if (connection.connected && allSelectedReady) {
                    "connected_public_gateway"
                } else if (!connection.connected && current.detail == "connected_public_gateway") {
                    connection.detail
                } else {
                    current.detail
                },
            )
        }
    }

    private suspend fun saveCompanionBinding(
        installationId: String,
        hardwareUid: String,
        companionId: String,
        deviceAddress: String? = null,
    ) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        if (current.installationId != installationId) {
            throw TransportException("mobile_relay_binding_changed")
        }
        val binding = MobileRelayCompanionBinding(hardwareUid, companionId, deviceAddress)
        val updated = current.copy(
            companionBindings = MobileRelaySettingsPolicy.bindCompanion(
                current.companionBindings,
                binding,
            ),
        )
        credentials.saveMobileRelaySettings(updated)
    }

    private suspend fun savePendingEnrollment(
        installationId: String,
        pending: MobileRelayPendingEnrollment,
    ) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        if (current.installationId != installationId) {
            throw TransportException("mobile_relay_binding_changed")
        }
        credentials.saveMobileRelaySettings(current.copy(pendingEnrollment = pending))
    }

    private suspend fun clearPendingEnrollment(
        installationId: String,
        hardwareUid: String,
        enrollmentId: String? = null,
    ) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        val pending = current.pendingEnrollment ?: return@withLock
        if (current.installationId != installationId || pending.hardwareUid != hardwareUid ||
            enrollmentId?.let { pending.enrollmentId != it } == true
        ) return@withLock
        credentials.saveMobileRelaySettings(current.copy(pendingEnrollment = null))
    }

    private suspend fun markActivationComplete(installationId: String) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        if (current.installationId != installationId || current.activationComplete) return@withLock
        credentials.saveMobileRelaySettings(current.copy(activationComplete = true))
    }

    private suspend fun markDeviceConfigured(
        installationId: String,
        deviceAddress: String,
    ) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        if (current.installationId != installationId) {
            throw TransportException("mobile_relay_binding_changed")
        }
        if (current.configuredDeviceAddresses.any {
                it.equals(deviceAddress, ignoreCase = true)
            }
        ) return@withLock
        credentials.saveMobileRelaySettings(
            current.copy(
                configuredDeviceAddresses = current.configuredDeviceAddresses
                    .plus(deviceAddress)
                    .takeLast(MAX_MOBILE_RELAY_DEVICES),
            ),
        )
    }

    private suspend fun pauseEnrollmentAttempt(installationId: String, code: String) =
        settingsMutex.withLock {
            val current = loadOrCreateSettings()
            if (current.installationId != installationId) return@withLock
            credentials.saveMobileRelaySettings(
                current.copy(enabled = false, activationComplete = false),
            )
            mutableState.update {
                it.copy(
                    enabled = false,
                    running = false,
                    detail = code,
                    enrollmentRemainingMillis = null,
                )
            }
        }

    @SuppressLint("MissingPermission")
    private fun bluetoothReadinessError(): String? {
        val permissions = if (Build.VERSION.SDK_INT >= 31) {
            listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (permissions.any {
                ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
            }
        ) return "bluetooth_permission_required"
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
            ?: return "bluetooth_unavailable"
        return if (adapter.isEnabled) null else "bluetooth_disabled"
    }

    private fun newRelayCredential(): String = Base64.getUrlEncoder().withoutPadding()
        .encodeToString(ByteArray(MobileRelayWirePolicy.RELAY_CREDENTIAL_BYTES).also(random::nextBytes))

    private fun startService() {
        runCatching { ContextCompat.startForegroundService(context, serviceIntent()) }
            .onFailure {
                SafeLog.warn("mobile_relay", "foreground_start_failed", it)
                disableAfterForegroundStartFailure("foreground_start_failed")
            }
    }

    private fun serviceIntent() = Intent(context, MobileRelayForegroundService::class.java)

    private fun verifyConfigureReceipt(receipt: MobileRelayReceipt) {
        if (receipt.schema != MobileRelayWirePolicy.RECEIPT_SCHEMA ||
            receipt.kind != "configure" || !receipt.accepted || receipt.nextOffset != 0 ||
            !receipt.complete || receipt.errorCode != null
        ) throw TransportException(receipt.errorCode ?: "mobile_relay_configure_rejected")
    }

    private fun verifyForgetReceipt(receipt: GatewayForgetReceipt) {
        if (receipt.schema != GATEWAY_FORGET_SCHEMA || !receipt.accepted ||
            receipt.state != "forgotten" || receipt.errorCode != null
        ) throw TransportException(receipt.errorCode ?: "gateway_forget_rejected")
    }

    private suspend fun stopForegroundRelayAndAwait() {
        context.stopService(serviceIntent())
        foregroundCompletion.await()
        runCatching { onSessionsClosed() }
        mutableState.update { current ->
            current.copy(
                running = false,
                backendConnected = false,
                teardownInProgress = false,
                selectedDeviceCount = if (current.enabled || current.cleanupInProgress) {
                    current.selectedDeviceCount
                } else {
                    0
                },
                enrollmentRemainingMillis = null,
                devices = current.devices.map {
                    val remainsSelected = current.cleanupInProgress ||
                        (current.enabled && it.selected)
                    it.copy(
                        selected = remainsSelected,
                        bluetoothConnected = false,
                        gatewayEnrolled = false,
                        detail = when {
                            current.cleanupInProgress -> it.detail
                            remainsSelected -> "disconnected"
                            else -> "off"
                        },
                        enrollmentRemainingMillis = null,
                    )
                },
            )
        }
    }

    private fun validateEnrollmentReceipt(
        receipt: GatewayEnrollmentReceipt,
        enrollmentId: String,
        allowPending: Boolean = false,
    ) {
        if (receipt.enrollmentId != enrollmentId ||
            (!receipt.accepted && !(allowPending &&
                receipt.state == "physical_confirmation_required"))
        ) throw TransportException(receipt.errorCode ?: "mobile_relay_enrollment_rejected")
    }

    private data class PendingDownlink(
        val actionId: String,
        val companionId: String,
        val exactFrame: ByteArray,
    )

    private data class EnrollmentCandidate(
        val bond: BondedCompanion,
        val hardwareUid: String,
        val displayName: String,
        val session: MobileRelayDeviceSession,
    )

    private class FixedBondCredentialStore(
        private val bond: BondedCompanion,
    ) : CredentialStore {
        override suspend fun bondedCompanion(): BondedCompanion = bond
        override suspend fun bondedCompanions(): List<BondedCompanion> = listOf(bond)
        override suspend fun saveBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun oauthTokens() = null
        override suspend fun saveOauthTokens(value: app.kitsu.mobile.security.OAuthTokens?) = Unit
    }

    companion object {
        fun fixedBondCredentials(bond: BondedCompanion): CredentialStore =
            FixedBondCredentialStore(bond)

        private fun completedSignal() = CompletableDeferred<Unit>().apply { complete(Unit) }

        private const val RETRY_MILLIS = 2_000L
        private const val DEVICE_POLL_MILLIS = 2_000L
        private const val RATE_LIMIT_RETRY_MILLIS = 60_000L
        private const val ENROLLMENT_POLL_MILLIS = 1_000L
        private const val DEFAULT_ENROLLMENT_WINDOW_MILLIS = 60_000
        private const val MAX_ENROLLMENT_WINDOW_MILLIS = 300_000
        private const val MAX_PENDING_DOWNLINKS = 32
        private const val MAX_DELIVERED_ACTION_IDS = 64
        private const val MAX_UPLINKS_PER_DEVICE_CYCLE = 4
        private const val GATEWAY_FORGET_SCHEMA = "kitsu.gateway-forget.v1"
        private val STALE_ENROLLMENT_CODES = setOf(
            "enrollment_unavailable",
            "physical_confirmation_required",
            "mobile_relay_enrollment_request_missing",
        )
        private val HARDWARE_UID = Regex("^KT[0-9A-F]{4}$")
    }
}
