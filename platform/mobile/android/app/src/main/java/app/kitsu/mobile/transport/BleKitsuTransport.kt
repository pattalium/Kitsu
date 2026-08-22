package app.kitsu.mobile.transport

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.GatewayConfigurationReceipt
import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import app.kitsu.mobile.model.GatewayEnrollmentReceipt
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WIRE_VERSION
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiRetryReceipt
import app.kitsu.mobile.model.toConfigureBody
import app.kitsu.mobile.pairing.ControllerPairingProgress
import app.kitsu.mobile.pairing.ControllerPairingProtocol
import app.kitsu.mobile.pairing.ControllerPairingService
import app.kitsu.mobile.pairing.ControllerPairingStage
import app.kitsu.mobile.pairing.PairingChannel
import app.kitsu.mobile.pairing.PairingException
import app.kitsu.mobile.security.BondedCompanion
import app.kitsu.mobile.security.CredentialStore
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.relay.MobileRelayBleOperations
import app.kitsu.mobile.relay.MobileRelayChunk
import app.kitsu.mobile.relay.MobileRelayDeviceSession
import app.kitsu.mobile.relay.MobileRelayPullKind
import app.kitsu.mobile.relay.MobileRelayPushKind
import app.kitsu.mobile.relay.MobileRelayReceipt
import app.kitsu.mobile.relay.MobileRelayWirePolicy
import app.kitsu.mobile.relay.MAX_MOBILE_RELAY_DEVICES
import java.util.Base64
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import kotlin.coroutines.resume
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

data class BleGattConfiguration(
    val service: UUID,
    val write: UUID,
    val notify: UUID,
)

class BleKitsuTransport(
    private val context: Context,
    private val credentials: CredentialStore,
    private val configuration: BleGattConfiguration,
    private val scanTimeoutMillis: Long = 8_000,
    private val mobileRelayOperations: MobileRelayBleOperations = MobileRelayBleOperations(),
    private val confirmPresenceByScan: Boolean = true,
) : KitsuTransport, ControllerPairingService, MobileRelayDeviceSession {
    override val mode: ConnectionMode = ConnectionMode.DIRECT_BLE

    private val manager = context.getSystemService(BluetoothManager::class.java)
    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
        // Frozen operation schemas are default-valued stored properties and must
        // still be emitted on the wire; no direct mutation body has optional defaults.
        encodeDefaults = true
    }
    private val requestMutex = Mutex()
    private val pairingMutex = Mutex()
    private data class PendingResponse(
        val operation: String,
        val deferred: CompletableDeferred<ByteArray>,
    )

    private sealed interface DeviceScan {
        data class Found(val device: BluetoothDevice) : DeviceScan
        data object Absent : DeviceScan
        data class Failed(val code: String) : DeviceScan
    }

    private val pending = ConcurrentHashMap<String, PendingResponse>()
    private val eventBus = MutableSharedFlow<EventEnvelope>(
        replay = 0,
        extraBufferCapacity = 64,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    private val frameDecoder = GattFrameDecoder()
    private val pairingFrameDecoder = GattFrameDecoder(
        maxPayloadBytes = ControllerPairingProtocol.MAX_FRAME_BYTES,
    )
    private val frameTimeoutHandler = android.os.Handler(context.mainLooper)
    private var frameTimeoutGeneration = 0L

    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var writeCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var notifyCharacteristic: BluetoothGattCharacteristic? = null
    @Volatile private var negotiatedMtu: Int = 23
    @Volatile private var writeCompletion: CompletableDeferred<Int>? = null
    @Volatile private var connectionReady: CompletableDeferred<ConnectResult>? = null
    @Volatile private var handshakeResponse: CompletableDeferred<ByteArray>? = null
    @Volatile private var pairingInbox: Channel<ByteArray>? = null
    @Volatile private var pairingJob: Job? = null
    @Volatile private var envelopeSession: SecureEnvelopeSession? = null
    @Volatile private var connectedDeviceAddress: String? = null
    @Volatile private var meshOneShotReady = false
    private var failedProofs = 0
    private var proofBackoffUntilMillis = 0L

    override suspend fun connect(): ConnectResult {
        try {
            credentials.savePendingBondedCompanion(null)
        } catch (failure: Throwable) {
            SafeLog.warn("ble_credentials", "pending_credential_cleanup_failed", failure)
            return ConnectResult.Failed("credential_store_failed")
        }
        val profile = try {
            credentials.bondedCompanion()
        } catch (failure: Throwable) {
            SafeLog.warn("ble_credentials", "credential_store_failed", failure)
            return ConnectResult.Failed("credential_store_failed")
        } ?: return ConnectResult.CompanionAbsent
        val missing = missingPermissions()
        if (missing.isNotEmpty()) return ConnectResult.PermissionRequired(missing)
        if (isConnectedTo(profile.deviceAddress)) {
            return try {
                synchronizeClock()
                ConnectResult.Connected
            } catch (cancelled: CancellationException) {
                disconnect()
                throw cancelled
            } catch (failure: Throwable) {
                SafeLog.warn("ble_clock", "clock_sync_failed", failure)
                disconnect()
                ConnectResult.Failed("clock_sync_failed")
            }
        }

        return try {
            connectWithPermission(profile)
        } catch (cancelled: CancellationException) {
            disconnect()
            throw cancelled
        } catch (security: SecurityException) {
            SafeLog.warn("ble_connect", "permission_denied", security)
            ConnectResult.PermissionRequired(requiredPermissions())
        } catch (failure: Throwable) {
            SafeLog.warn("ble_connect", "direct_connect_failed", failure)
            ConnectResult.Failed("direct_connect_failed")
        }
    }

    override suspend fun pairController(
        label: String,
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion = pairingMutex.withLock {
        if (credentials.bondedCompanions().size >= MAX_MOBILE_RELAY_DEVICES) {
            throw PairingException("controller_device_limit")
        }
        // A candidate left by process death was never promoted by a verified pair_ok.
        credentials.savePendingBondedCompanion(null)
        val missing = missingPermissions()
        if (missing.isNotEmpty()) throw PairingException("bluetooth_permission_required")
        val activeJob = currentCoroutineContext()[Job]
            ?: throw PairingException("pairing_failed")
        pairingJob = activeJob
        var committed = false
        try {
            val result = pairControllerWithPermission(label) { progress ->
                runCatching { onProgress(progress) }
            }
            committed = true
            runCatching {
                onProgress(ControllerPairingProgress(ControllerPairingStage.COMPLETE, "controller_paired"))
            }
            result
        } catch (failure: CancellationException) {
            if (!committed) withContext(NonCancellable) {
                credentials.savePendingBondedCompanion(null)
            }
            withContext(NonCancellable) {
                runCatching {
                    onProgress(ControllerPairingProgress(ControllerPairingStage.CANCELLED, "pairing_cancelled"))
                }
            }
            throw PairingException("pairing_cancelled", failure)
        } catch (failure: Throwable) {
            if (!committed) withContext(NonCancellable) {
                credentials.savePendingBondedCompanion(null)
            }
            throw failure
        } finally {
            withContext(NonCancellable) { disconnect() }
            if (pairingJob === activeJob) pairingJob = null
        }
    }

    override fun cancelPairing() {
        pairingJob?.cancel(CancellationException("pairing_cancelled"))
    }

    @SuppressLint("MissingPermission")
    private suspend fun pairControllerWithPermission(
        label: String,
        report: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion {
        disconnect()
        val adapter = manager?.adapter ?: throw PairingException("bluetooth_unavailable")
        if (!adapter.isEnabled) throw PairingException("bluetooth_disabled")
        scanPrerequisiteError()?.let { throw PairingException(it) }

        report(ControllerPairingProgress(ControllerPairingStage.SCANNING, "scanning_pair_phone"))
        val device = when (val scan = scanForPairingCandidate()) {
            is DeviceScan.Found -> scan.device
            DeviceScan.Absent -> throw PairingException("pairing_device_absent")
            is DeviceScan.Failed -> throw PairingException(scan.code)
        }

        report(
            ControllerPairingProgress(
                ControllerPairingStage.OS_SECURE_PAIRING,
                "compare_codes_accept_android_then_hold_prg_if_same",
            ),
        )
        if (!ensureBonded(device)) throw PairingException("secure_bond_failed")

        report(ControllerPairingProgress(ControllerPairingStage.CONNECTING_GATT, "opening_secure_gatt"))
        val ready = CompletableDeferred<ConnectResult>()
        connectionReady = ready
        negotiatedMtu = 23
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        val result = withTimeoutOrNull(CONNECT_TIMEOUT_MILLIS) { ready.await() }
            ?: ConnectResult.Failed("gatt_timeout")
        if (result != ConnectResult.Connected) throw PairingException(
            (result as? ConnectResult.Failed)?.code ?: "gatt_connect_failed",
        )

        val inbox = Channel<ByteArray>(capacity = PAIRING_INBOX_CAPACITY)
        pairingInbox = inbox
        val channel = object : PairingChannel {
            override suspend fun send(payload: ByteArray) {
                if (payload.isEmpty() || payload.size > ControllerPairingProtocol.MAX_FRAME_BYTES) {
                    throw PairingException("pairing_failed")
                }
                val activeGatt = gatt ?: throw PairingException("gatt_disconnected")
                val characteristic = writeCharacteristic ?: throw PairingException("gatt_disconnected")
                if (!writeFrame(activeGatt, characteristic, encodeGattFrame(payload))) {
                    throw PairingException("gatt_write_failed")
                }
            }

            override suspend fun receive(): ByteArray = try {
                inbox.receive()
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Throwable) {
                throw PairingException("gatt_disconnected", failure)
            }
        }

        var stored: BondedCompanion? = null
        try {
            ControllerPairingProtocol().pair(
                label = label,
                channel = channel,
                persistCandidate = { grant ->
                    val displayName = runCatching { device.name }
                        .getOrNull()
                        ?.trim()
                        ?.takeIf(String::isNotEmpty)
                        ?: grant.deviceUid
                    val candidate = BondedCompanion(
                        deviceAddress = device.address,
                        displayName = displayName,
                        controllerIdB64 = grant.controllerIdB64,
                        controllerRootB64 = grant.rootB64,
                    )
                    credentials.savePendingBondedCompanion(candidate)
                    stored = candidate
                },
                deleteCandidate = {
                    credentials.savePendingBondedCompanion(null)
                    stored = null
                },
                onPending = { expiresInMillis ->
                    report(
                        ControllerPairingProgress(
                            ControllerPairingStage.WAITING_FOR_PRG,
                            "hold_prg_to_confirm_${expiresInMillis}ms",
                        ),
                    )
                },
                onCandidateStored = {
                    report(
                        ControllerPairingProgress(
                            ControllerPairingStage.SAVING_CONTROLLER,
                            "committing_controller",
                        ),
                    )
                },
            )
            val verified = stored ?: throw PairingException("credential_store_failed")
            credentials.saveBondedCompanion(verified)
            runCatching { credentials.savePendingBondedCompanion(null) }
                .onFailure { SafeLog.warn("ble_pairing", "pending_cleanup_failed") }
            return verified
        } finally {
            if (pairingInbox === inbox) pairingInbox = null
            inbox.close()
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun connectWithPermission(profile: BondedCompanion): ConnectResult {
        if (android.os.SystemClock.elapsedRealtime() < proofBackoffUntilMillis) {
            return ConnectResult.Failed("controller_auth_backoff")
        }
        val adapter = manager?.adapter ?: return ConnectResult.Failed("bluetooth_unavailable")
        if (!adapter.isEnabled) return ConnectResult.Failed("bluetooth_disabled")
        scanPrerequisiteError()?.let { return ConnectResult.Failed(it) }

        val bonded = adapter.bondedDevices.firstOrNull {
            it.address.equals(profile.deviceAddress, ignoreCase = true) &&
                it.bondState == BluetoothDevice.BOND_BONDED
        } ?: return ConnectResult.Failed("bond_missing_repair_required")

        val seen = if (confirmPresenceByScan) {
            when (val scan = scanForKnown(bonded.address)) {
                is DeviceScan.Found -> scan.device
                DeviceScan.Absent -> return ConnectResult.CompanionAbsent
                is DeviceScan.Failed -> return ConnectResult.Failed(scan.code)
            }
        } else {
            bonded
        }
        val ready = CompletableDeferred<ConnectResult>()
        connectionReady = ready
        negotiatedMtu = 23
        gatt = seen.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        val result = withTimeoutOrNull(CONNECT_TIMEOUT_MILLIS) { ready.await() }
            ?: ConnectResult.Failed("gatt_timeout")
        if (result != ConnectResult.Connected) {
            disconnect()
            return result
        }

        val controllerId = runCatching {
            SecureEnvelopeSession.decodeUrl(profile.controllerIdB64, "invalid_controller_id")
                .also { require(it.size == 16) }
        }.getOrElse {
            disconnect()
            return ConnectResult.Failed("invalid_controller_capability")
        }
        val controllerRoot = runCatching {
            SecureEnvelopeSession.decodeUrl(profile.controllerRootB64, "invalid_controller_root")
                .also { require(it.size == 32) }
        }.getOrElse {
            disconnect()
            return ConnectResult.Failed("invalid_controller_capability")
        }
        val session = withTimeoutOrNull(CapabilityHandshake.HANDSHAKE_TIMEOUT_MILLIS) {
            runCatching {
                CapabilityHandshake().perform(controllerId, controllerRoot, ::exchangeHandshake)
            }.getOrNull()
        }
        controllerRoot.fill(0)
        if (session == null) {
            failedProofs++
            if (failedProofs >= MAX_FAILED_PROOFS) {
                proofBackoffUntilMillis = android.os.SystemClock.elapsedRealtime() + PROOF_BACKOFF_MILLIS
                failedProofs = 0
            }
            SafeLog.warn("ble_auth", "controller_auth_failed")
            disconnect()
            return ConnectResult.Failed("controller_auth_failed")
        }
        failedProofs = 0
        envelopeSession = session
        connectedDeviceAddress = profile.deviceAddress
        try {
            synchronizeClock()
        } catch (cancelled: CancellationException) {
            disconnect()
            throw cancelled
        } catch (failure: Throwable) {
            SafeLog.warn("ble_clock", "clock_sync_failed", failure)
            disconnect()
            return ConnectResult.Failed("clock_sync_failed")
        }
        SafeLog.info("ble_connected", mapOf("device" to profile.displayName))
        return ConnectResult.Connected
    }

    @SuppressLint("MissingPermission")
    private suspend fun scanForKnown(address: String): DeviceScan {
        val scanner = manager?.adapter?.bluetoothLeScanner
            ?: return DeviceScan.Failed("scanner_unavailable")
        return withContext(Dispatchers.Main.immediate) {
            suspendCancellableCoroutine { continuation ->
                var completed = false
                lateinit var callback: ScanCallback
                val handler = android.os.Handler(context.mainLooper)
                val finish: (DeviceScan) -> Unit = { result ->
                    if (!completed) {
                        completed = true
                        handler.removeCallbacksAndMessages(null)
                        runCatching { scanner.stopScan(callback) }
                        if (continuation.isActive) continuation.resume(result)
                    }
                }
                callback = object : ScanCallback() {
                    override fun onScanResult(callbackType: Int, result: ScanResult) {
                        if (result.device.address.equals(address, ignoreCase = true)) {
                            finish(DeviceScan.Found(result.device))
                        }
                    }

                    override fun onScanFailed(errorCode: Int) {
                        SafeLog.warn("ble_scan", "scan_failed_$errorCode")
                        finish(DeviceScan.Failed("scan_failed_$errorCode"))
                    }
                }
                val filter = ScanFilter.Builder()
                    .setDeviceAddress(address)
                    .setServiceUuid(ParcelUuid(configuration.service))
                    .build()
                try {
                    scanner.startScan(
                        listOf(filter),
                        ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
                        callback,
                    )
                    handler.postDelayed({ finish(DeviceScan.Absent) }, scanTimeoutMillis)
                } catch (failure: Throwable) {
                    SafeLog.warn("ble_scan", "scan_start_failed", failure)
                    finish(DeviceScan.Failed("scan_start_failed"))
                }
                continuation.invokeOnCancellation {
                    if (!completed) {
                        completed = true
                        handler.removeCallbacksAndMessages(null)
                        runCatching { scanner.stopScan(callback) }
                    }
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun scanForPairingCandidate(): DeviceScan {
        val scanner = manager?.adapter?.bluetoothLeScanner
            ?: return DeviceScan.Failed("scanner_unavailable")
        return withContext(Dispatchers.Main.immediate) {
            val found = CompletableDeferred<DeviceScan>()
            val scanCallback = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    found.complete(DeviceScan.Found(result.device))
                }

                override fun onScanFailed(errorCode: Int) {
                    SafeLog.warn("ble_pair_scan", "scan_failed_$errorCode")
                    found.complete(DeviceScan.Failed("scan_failed_$errorCode"))
                }
            }
            val filter = ScanFilter.Builder()
                .setServiceUuid(ParcelUuid(configuration.service))
                .build()
            try {
                scanner.startScan(
                    listOf(filter),
                    ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
                    scanCallback,
                )
                withTimeoutOrNull(scanTimeoutMillis) { found.await() } ?: DeviceScan.Absent
            } catch (failure: CancellationException) {
                throw failure
            } catch (failure: Throwable) {
                SafeLog.warn("ble_pair_scan", "scan_start_failed", failure)
                DeviceScan.Failed("scan_start_failed")
            } finally {
                runCatching { scanner.stopScan(scanCallback) }
            }
        }
    }

    /**
     * Android 6-11 couple BLE scan discovery to the system Location Services
     * switch even when the app already has location permission. Treating that
     * condition as an empty scan would incorrectly authorize remote fallback.
     */
    @Suppress("DEPRECATION")
    private fun scanPrerequisiteError(): String? {
        if (Build.VERSION.SDK_INT > Build.VERSION_CODES.R) return null
        val location = context.getSystemService(LocationManager::class.java)
            ?: return "location_services_unavailable"
        val enabled = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            location.isLocationEnabled
        } else {
            runCatching {
                location.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                    location.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
            }.getOrDefault(false)
        }
        return if (enabled) null else "location_services_disabled"
    }

    @SuppressLint("MissingPermission")
    private suspend fun ensureBonded(device: BluetoothDevice): Boolean {
        // Android exposes bond state but no trustworthy public LESC/encryption
        // attestation. The encrypted GATT service and Kitsu's issuance gate enforce it.
        if (device.bondState == BluetoothDevice.BOND_BONDED) return true
        return withContext(Dispatchers.Main.immediate) {
            val result = CompletableDeferred<Boolean>()
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                    @Suppress("DEPRECATION")
                    val changed = if (Build.VERSION.SDK_INT >= 33) {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    } else {
                        intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    }
                    if (changed?.address?.equals(device.address, ignoreCase = true) != true) return
                    when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                        BluetoothDevice.BOND_BONDED -> result.complete(true)
                        BluetoothDevice.BOND_NONE -> result.complete(false)
                    }
                }
            }
            try {
                if (Build.VERSION.SDK_INT >= 33) {
                    context.registerReceiver(
                        receiver,
                        IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
                        Context.RECEIVER_EXPORTED,
                    )
                } else {
                    @Suppress("DEPRECATION")
                    context.registerReceiver(receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
                }
                if (device.bondState == BluetoothDevice.BOND_BONDED) return@withContext true
                if (device.bondState == BluetoothDevice.BOND_NONE) {
                    // Android's public SDK exposes only createBond(). Because this
                    // BluetoothDevice was discovered by the LE-only Kitsu scan,
                    // the platform selects the LE transport for the bond.
                    val started = device.createBond()
                    if (!started && device.bondState !in listOf(
                            BluetoothDevice.BOND_BONDING,
                            BluetoothDevice.BOND_BONDED,
                        )
                    ) return@withContext false
                }
                withTimeoutOrNull(OS_BOND_TIMEOUT_MILLIS) { result.await() } ?: false
            } finally {
                runCatching { context.unregisterReceiver(receiver) }
            }
        }
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when {
                newState == BluetoothProfile.STATE_DISCONNECTED -> {
                    if (this@BleKitsuTransport.gatt === gatt) {
                        this@BleKitsuTransport.gatt = null
                        negotiatedMtu = 23
                        writeCharacteristic = null
                        notifyCharacteristic = null
                        envelopeSession = null
                        connectedDeviceAddress = null
                        connectionReady?.complete(ConnectResult.Failed("gatt_status_$status"))
                        pairingInbox?.close(PairingException("gatt_disconnected"))
                        failPending("gatt_disconnected")
                    }
                    runCatching { gatt.close() }
                }
                status != BluetoothGatt.GATT_SUCCESS ->
                    connectionReady?.complete(ConnectResult.Failed("gatt_status_$status"))
                newState == BluetoothProfile.STATE_CONNECTED -> {
                    val mtuStarted = runCatching { gatt.requestMtu(517) }.getOrDefault(false)
                    if (!mtuStarted && !gatt.discoverServices()) {
                        connectionReady?.complete(ConnectResult.Failed("service_discovery_start_failed"))
                    }
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                connectionReady?.complete(ConnectResult.Failed("service_discovery_failed"))
                return
            }
            val service: BluetoothGattService? = gatt.getService(configuration.service)
            if (service == null) {
                connectionReady?.complete(ConnectResult.Failed("service_missing"))
                return
            }
            val writer = service.getCharacteristic(configuration.write)
            if (writer == null) {
                connectionReady?.complete(ConnectResult.Failed("write_characteristic_missing"))
                return
            }
            val notifier = service.getCharacteristic(configuration.notify)
            if (notifier == null) {
                connectionReady?.complete(ConnectResult.Failed("notify_characteristic_missing"))
                return
            }
            if (!gatt.setCharacteristicNotification(notifier, true)) {
                connectionReady?.complete(ConnectResult.Failed("notify_enable_failed"))
                return
            }
            notifyCharacteristic = notifier
            val descriptor = notifier.getDescriptor(CLIENT_CONFIGURATION_UUID)
            if (descriptor == null) {
                connectionReady?.complete(ConnectResult.Failed("notify_descriptor_missing"))
                return
            }
            writeCharacteristic = writer
            val started = if (Build.VERSION.SDK_INT >= 33) {
                gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                    BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    gatt.writeDescriptor(descriptor)
                }
            }
            if (!started) connectionReady?.complete(ConnectResult.Failed("notify_descriptor_write_failed"))
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid != CLIENT_CONFIGURATION_UUID) return
            connectionReady?.complete(
                if (status == BluetoothGatt.GATT_SUCCESS) ConnectResult.Connected
                else ConnectResult.Failed("notify_descriptor_write_failed"),
            )
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) negotiatedMtu = mtu.coerceAtLeast(23)
            val started = try {
                gatt.discoverServices()
            } catch (_: SecurityException) {
                false
            }
            if (!started) {
                connectionReady?.complete(ConnectResult.Failed("service_discovery_start_failed"))
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            writeCompletion?.complete(status)
        }

        @Deprecated("Deprecated by Android")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            acceptBytes(characteristic.value ?: return)
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            acceptBytes(value)
        }
    }

    private fun acceptBytes(bytes: ByteArray) {
        val activeDecoder = if (pairingInbox != null) pairingFrameDecoder else frameDecoder
        val result = activeDecoder.accept(bytes, android.os.SystemClock.elapsedRealtime())
        result.error?.let { code ->
            failPending(code)
            closeMalformedGatt(code)
            return
        }
        result.frames.forEach { payload -> acceptPayload(payload) }
        scheduleFrameExpiryIfNeeded(activeDecoder)
    }

    private fun acceptPayload(payload: ByteArray) {
        pairingInbox?.let { inbox ->
            if (payload.isEmpty() || payload.size > ControllerPairingProtocol.MAX_FRAME_BYTES ||
                !inbox.trySend(payload).isSuccess
            ) {
                inbox.close(PairingException("pairing_failed"))
                closeMalformedGatt("invalid_pairing_frame")
            }
            return
        }
        handshakeResponse?.let { waiting ->
            if (payload.size > CapabilityHandshake.MAX_HANDSHAKE_BYTES) {
                waiting.completeExceptionally(HandshakeException("invalid_handshake_frame"))
            } else {
                waiting.complete(payload)
            }
            return
        }

        val verified = try {
            envelopeSession?.decodeIncoming(payload) ?: throw EnvelopeException("session_not_authenticated")
        } catch (failure: EnvelopeException) {
            failPending(failure.code)
            closeMalformedGatt(failure.code)
            return
        }
        if (verified.channel == SecureEnvelopeSession.CHANNEL_RESPONSE) {
            try {
                val responseId = verified.requestId.toString()
                val pendingResponse = pending.remove(responseId)
                if (pendingResponse == null ||
                    pendingResponse.operation != verified.operation
                ) {
                    throw EnvelopeException("response_binding_failed")
                }
                pendingResponse.deferred.complete(verified.payload)
                return
            } catch (failure: Throwable) {
                val code = (failure as? EnvelopeException)?.code ?: "malformed_response"
                failPending(code)
                closeMalformedGatt(code)
                return
            }
        }
        if (verified.channel == SecureEnvelopeSession.CHANNEL_EVENT &&
            verified.operation == "gateway.enroll.event"
        ) {
            try {
                FirmwareBlePayloadMapper.gatewayEnrollmentEvent(verified.payload)
            } catch (_: Throwable) {
                failPending("malformed_enrollment_event")
                closeMalformedGatt("malformed_enrollment_event")
            }
            return
        }
        val line = verified.payload.toString(Charsets.UTF_8)
        try {
            val event = json.decodeFromString(EventEnvelope.serializer(), line)
            if (event.v == WIRE_VERSION && verified.channel == SecureEnvelopeSession.CHANNEL_EVENT) {
                eventBus.tryEmit(event)
            } else {
                throw EnvelopeException("invalid_event_binding")
            }
        } catch (failure: Throwable) {
            failPending("malformed_event")
            closeMalformedGatt("malformed_event")
        }
    }

    @Synchronized
    private fun scheduleFrameExpiryIfNeeded(activeDecoder: GattFrameDecoder) {
        val generation = ++frameTimeoutGeneration
        if (!activeDecoder.hasPartialFrame()) return
        frameTimeoutHandler.postDelayed({
            synchronized(this) {
                if (generation != frameTimeoutGeneration) return@synchronized
                if (activeDecoder.expire(android.os.SystemClock.elapsedRealtime())) {
                    failPending("frame_timeout")
                    closeMalformedGatt("frame_timeout")
                }
            }
        }, GATT_FRAME_TIMEOUT_MILLIS)
    }

    @SuppressLint("MissingPermission")
    private fun closeMalformedGatt(code: String) {
        SafeLog.warn("ble_frame", code)
        val active = gatt
        gatt = null
        writeCharacteristic = null
        notifyCharacteristic = null
        negotiatedMtu = 23
        envelopeSession = null
        meshOneShotReady = false
        frameDecoder.clear()
        pairingFrameDecoder.clear()
        frameTimeoutGeneration += 1
        handshakeResponse?.completeExceptionally(EnvelopeException(code))
        handshakeResponse = null
        pairingInbox?.close(PairingException(code))
        pairingInbox = null
        runCatching { active?.disconnect() }
        runCatching { active?.close() }
    }

    private suspend fun request(op: String, body: JsonObject): ByteArray = requestMutex.withLock {
        val activeGatt = gatt ?: throw TransportException("not_connected")
        val characteristic = writeCharacteristic ?: throw TransportException("not_connected")
        val session = envelopeSession ?: throw TransportException("session_not_authenticated")
        val requestId = UUID.randomUUID()
        val id = requestId.toString()
        // request_id and op are already authenticated outer-envelope fields;
        // the compact firmware contract signs only the operation body here.
        val payload = body.toString().toByteArray(Charsets.UTF_8)
        val secured = try {
            try {
                session.encodeRequest(requestId, op, payload)
            } catch (failure: EnvelopeException) {
                throw TransportException(failure.code, failure)
            }
        } finally {
            // Provisioning bodies can contain a Wi-Fi passphrase. Do not leave the
            // temporary plaintext JSON byte array live after authenticated wrapping.
            payload.fill(0)
        }
        val frame = encodeGattFrame(secured)

        val response = CompletableDeferred<ByteArray>()
        pending[id] = PendingResponse(op, response)
        val started = writeFrame(activeGatt, characteristic, frame)
        if (!started) {
            pending.remove(id)
            throw TransportException("gatt_write_failed")
        }
        withTimeoutOrNull(REQUEST_TIMEOUT_MILLIS) { response.await() }
            ?: run {
                pending.remove(id)
                throw TransportException("request_timeout")
            }
    }

    private suspend fun exchangeHandshake(payload: ByteArray): ByteArray {
        if (payload.isEmpty() || payload.size > CapabilityHandshake.MAX_HANDSHAKE_BYTES) {
            throw HandshakeException("invalid_handshake_frame")
        }
        val activeGatt = gatt ?: throw HandshakeException("gatt_disconnected")
        val characteristic = writeCharacteristic ?: throw HandshakeException("gatt_disconnected")
        val response = CompletableDeferred<ByteArray>()
        handshakeResponse = response
        val started = writeFrame(activeGatt, characteristic, encodeGattFrame(payload))
        if (!started) {
            handshakeResponse = null
            throw HandshakeException("gatt_write_failed")
        }
        return try {
            withTimeoutOrNull(WRITE_TIMEOUT_MILLIS) { response.await() }
                ?: throw HandshakeException("handshake_timeout")
        } finally {
            handshakeResponse = null
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun writeFrame(
        activeGatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        frame: ByteArray,
    ): Boolean {
        val chunkBytes = (negotiatedMtu - 3).coerceIn(20, 512)
        var offset = 0
        while (offset < frame.size) {
            val end = minOf(offset + chunkBytes, frame.size)
            val completion = CompletableDeferred<Int>()
            writeCompletion = completion
            val started = if (Build.VERSION.SDK_INT >= 33) {
                activeGatt.writeCharacteristic(
                    characteristic,
                    frame.copyOfRange(offset, end),
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    characteristic.value = frame.copyOfRange(offset, end)
                    activeGatt.writeCharacteristic(characteristic)
                }
            }
            if (!started) {
                writeCompletion = null
                return false
            }
            val status = withTimeoutOrNull(WRITE_TIMEOUT_MILLIS) { completion.await() }
            writeCompletion = null
            if (status != BluetoothGatt.GATT_SUCCESS) return false
            offset = end
        }
        return true
    }

    private suspend fun successfulPayload(op: String, body: JsonObject): ByteArray {
        val response = request(op, body)
        FirmwareBlePayloadMapper.rejectionCode(response)?.let { throw TransportException(it) }
        return response
    }

    private suspend inline fun <reified T> requestBody(op: String, body: JsonObject): T {
        val response = successfulPayload(op, body)
        return try {
            json.decodeFromJsonElement(json.parseToJsonElement(response.toString(Charsets.UTF_8)))
        } catch (failure: Throwable) {
            throw TransportException("malformed_response", failure)
        }
    }

    override suspend fun status(): KitsuStatus = FirmwareBlePayloadMapper.state(
        successfulPayload("state.get", buildJsonObject {}),
        java.time.Instant.now().epochSecond,
    ).also { meshOneShotReady = it.mesh.oneShotReady }

    override suspend fun history(after: String?, limit: Int): HistoryPage =
        FirmwareBlePayloadMapper.history(
            successfulPayload("history.get", buildJsonObject {
            put("after", after)
            put("limit", boundedLimit(limit))
            }),
        )

    override suspend fun peers(): PeerPage = FirmwareBlePayloadMapper.peers(
        successfulPayload("peers.get", buildJsonObject {
            put("after", null as String?)
            put("limit", 100)
        }),
    )

    override suspend fun messages(after: String?, limit: Int): MessagePage =
        FirmwareBlePayloadMapper.messages(
            successfulPayload("messages.get", buildJsonObject {
            put("after", after)
            put("limit", boundedLimit(limit))
            }),
        )

    override suspend fun channels(): List<MeshChannel> = FirmwareBlePayloadMapper.channels(
        successfulPayload("channels.get", buildJsonObject {}),
    )

    override suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt =
        FirmwareBlePayloadMapper.meshConfiguration(
            successfulPayload("mesh.configure", buildJsonObject { put("enabled", enabled) }),
        )

    override suspend fun action(command: ActionCommand): ActionReceipt {
        if (command.kind !in setOf(
                app.kitsu.mobile.model.ActionKind.PET,
                app.kitsu.mobile.model.ActionKind.FEED,
            app.kitsu.mobile.model.ActionKind.PLAY,
            app.kitsu.mobile.model.ActionKind.LISTEN_ONCE,
            app.kitsu.mobile.model.ActionKind.SEND_MESSAGE,
            )
        ) throw TransportException("firmware_operation_unavailable")
        val direct = DirectActionPreparer(
            synchronizeClock = ::synchronizeClock,
            currentEpochSeconds = { java.time.Instant.now().epochSecond },
            messageOneShotReady = { meshOneShotReady },
        ).prepare(command)
        return requestBody("action.apply", json.encodeToJsonElement(direct) as JsonObject)
    }

    override suspend fun provisionWifi(credentials: WifiProvisioning): ProvisioningReceipt {
        ProvisioningPolicy.wifiError(credentials)?.let { throw TransportException(it) }
        val body = json.encodeToJsonElement(credentials.toConfigureBody()) as JsonObject
        return DirectProvisioningPlan.execute(
            writeOperation = "wifi.configure",
            synchronizeClock = ::synchronizeClock,
        ) { operation ->
            FirmwareBlePayloadMapper.wifiConfiguration(successfulPayload(operation, body))
        }
    }

    override suspend fun retryWifi(): WifiRetryReceipt =
        FirmwareBlePayloadMapper.wifiRetry(
            successfulPayload("wifi.retry", buildJsonObject {}),
        )

    override suspend fun configureGateway(
        configuration: GatewayConfiguration,
    ): GatewayConfigurationReceipt {
        ProvisioningPolicy.gatewayError(configuration)?.let { throw TransportException(it) }
        val body = json.encodeToJsonElement(configuration) as JsonObject
        return DirectProvisioningPlan.execute(
            writeOperation = "gateway.configure",
            synchronizeClock = ::synchronizeClock,
        ) { operation ->
            FirmwareBlePayloadMapper.gatewayConfiguration(successfulPayload(operation, body))
        }
    }

    override suspend fun beginGatewayEnrollment(
        request: GatewayEnrollmentBeginBody,
    ): GatewayEnrollmentReceipt {
        EnrollmentPolicy.beginError(request)?.let { throw TransportException(it) }
        val body = json.encodeToJsonElement(request) as JsonObject
        return DirectProvisioningPlan.execute(
            writeOperation = "gateway.enroll.begin",
            synchronizeClock = ::synchronizeClock,
        ) { operation ->
            FirmwareBlePayloadMapper.gatewayEnrollmentBegin(
                successfulPayload(operation, body),
                request.enrollmentId,
            )
        }
    }

    override suspend fun finishGatewayEnrollment(
        request: GatewayEnrollmentFinishBody,
    ): GatewayEnrollmentReceipt {
        EnrollmentPolicy.finishError(request)?.let { throw TransportException(it) }
        val body = json.encodeToJsonElement(request) as JsonObject
        return FirmwareBlePayloadMapper.gatewayEnrollmentFinish(
            successfulPayload("gateway.enroll.finish", body),
            request.enrollmentId,
        )
    }

    override suspend fun beginEnrollment(
        enrollmentId: String,
        claimToken: String,
    ): GatewayEnrollmentReceipt = beginGatewayEnrollment(
        GatewayEnrollmentBeginBody(enrollmentId = enrollmentId, claimToken = claimToken),
    )

    override suspend fun finishEnrollment(enrollmentId: String): GatewayEnrollmentReceipt =
        finishGatewayEnrollment(GatewayEnrollmentFinishBody(enrollmentId = enrollmentId))

    override suspend fun configureRelay(
        gatewayId: String,
        caCertificateDerB64: String,
    ): MobileRelayReceipt = requestBody(
        mobileRelayOperations.exchange,
        buildJsonObject {
            put("schema", MobileRelayWirePolicy.EXCHANGE_SCHEMA)
            put("kind", "relay_configure")
            put("gateway_id", gatewayId)
            put("ca_cert_der_b64", caCertificateDerB64)
        },
    )

    override suspend fun pull(kind: MobileRelayPullKind, offset: Int): MobileRelayChunk {
        val response = successfulPayload(
            mobileRelayOperations.exchange,
            buildJsonObject {
                put("schema", MobileRelayWirePolicy.EXCHANGE_SCHEMA)
                put("kind", kind.wireName)
                put("offset", offset)
            },
        )
        return try {
            val body = json.parseToJsonElement(response.toString(Charsets.UTF_8))
            if ((body as? JsonObject)?.get("schema")?.jsonPrimitive?.contentOrNull ==
                MobileRelayWirePolicy.RECEIPT_SCHEMA
            ) {
                val receipt = json.decodeFromJsonElement<MobileRelayReceipt>(body)
                throw TransportException(receipt.errorCode ?: "relay_pull_rejected")
            }
            json.decodeFromJsonElement(body)
        } catch (failure: TransportException) {
            throw failure
        } catch (failure: Throwable) {
            throw TransportException("malformed_response", failure)
        }
    }

    override suspend fun push(
        kind: MobileRelayPushKind,
        offset: Int,
        total: Int,
        data: ByteArray,
        final: Boolean,
    ): MobileRelayReceipt = requestBody(
        mobileRelayOperations.exchange,
        buildJsonObject {
            put("schema", MobileRelayWirePolicy.EXCHANGE_SCHEMA)
            put("kind", kind.wireName)
            put("offset", offset)
            put("total", total)
            put("data_b64", Base64.getUrlEncoder().withoutPadding().encodeToString(data))
            put("final", final)
        },
    )

    override fun events(after: String?): Flow<EventEnvelope> = flow { emitAll(eventBus) }

    /** Lets the foreground public-gateway service take over an already-authenticated
     * GATT session without disconnecting and immediately trying to scan it again. */
    fun isConnectedTo(deviceAddress: String): Boolean =
        connectedDeviceAddress?.equals(deviceAddress, ignoreCase = true) == true &&
            gatt != null && writeCharacteristic != null && notifyCharacteristic != null &&
            envelopeSession != null

    @SuppressLint("MissingPermission")
    override suspend fun disconnect() {
        val active = gatt
        val notifier = notifyCharacteristic
        gatt = null
        writeCharacteristic = null
        notifyCharacteristic = null
        negotiatedMtu = 23
        envelopeSession = null
        connectedDeviceAddress = null
        meshOneShotReady = false
        handshakeResponse?.completeExceptionally(HandshakeException("disconnected"))
        handshakeResponse = null
        pairingInbox?.close(PairingException("disconnected"))
        pairingInbox = null
        frameDecoder.clear()
        pairingFrameDecoder.clear()
        writeCompletion?.completeExceptionally(TransportException("disconnected"))
        writeCompletion = null
        connectionReady = null
        failPending("disconnected")
        runCatching {
            if (active != null && notifier != null) {
                active.setCharacteristicNotification(notifier, false)
            }
        }
        runCatching { active?.disconnect() }
        runCatching { active?.close() }
    }

    private suspend fun synchronizeClock() {
        successfulPayload("clock.sync", buildJsonObject {
            put("epoch", java.time.Instant.now().epochSecond)
        })
    }

    private fun failPending(code: String) {
        pending.values.forEach { it.deferred.completeExceptionally(TransportException(code)) }
        pending.clear()
    }

    private fun missingPermissions(): List<String> = requiredPermissions().filter {
        ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
    }

    private fun requiredPermissions(): List<String> = when {
        Build.VERSION.SDK_INT >= 31 -> listOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
        )
        else -> listOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    companion object {
        private val CLIENT_CONFIGURATION_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val CONNECT_TIMEOUT_MILLIS = 12_000L
        private const val OS_BOND_TIMEOUT_MILLIS = 90_000L
        private const val REQUEST_TIMEOUT_MILLIS = 10_000L
        private const val WRITE_TIMEOUT_MILLIS = 5_000L
        private const val PAIRING_INBOX_CAPACITY = 4
        private const val MAX_FAILED_PROOFS = 3
        private const val PROOF_BACKOFF_MILLIS = 30_000L
    }
}
