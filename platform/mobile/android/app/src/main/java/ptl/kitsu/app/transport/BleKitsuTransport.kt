package ptl.kitsu.app.transport

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
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.ControllerForgetReceipt
import ptl.kitsu.app.model.ENCOUNTER_CODES_OPERATION
import ptl.kitsu.app.model.ENCOUNTER_CATALOG_OPERATION
import ptl.kitsu.app.model.ENCOUNTER_NEIGHBORS_OPERATION
import ptl.kitsu.app.model.EncounterCatalogPage
import ptl.kitsu.app.model.EncounterCodePage
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.model.EventEnvelope
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.FUN_EXPEDITION_CLAIM_OPERATION
import ptl.kitsu.app.model.FUN_EXPEDITION_START_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_BEGIN_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_CHOOSE_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_HOST_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_JOIN_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_LEAVE_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_SCAN_OPERATION
import ptl.kitsu.app.model.FUN_STATE_GET_OPERATION
import ptl.kitsu.app.model.FUN_STORY_ADVANCE_OPERATION
import ptl.kitsu.app.model.FUN_STORY_CHOOSE_OPERATION
import ptl.kitsu.app.model.FUN_STORY_START_OPERATION
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.HistoryPage
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.MessageMarkReadReceipt
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshConfigurationReceipt
import ptl.kitsu.app.model.NearbyKitsuPage
import ptl.kitsu.app.model.NEIGHBOR_ACTION_OPERATION
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionReceipt
import ptl.kitsu.app.model.PeerPage
import ptl.kitsu.app.model.PartyJoinCommand
import ptl.kitsu.app.model.PartyRoundCommand
import ptl.kitsu.app.model.StoryTrigger
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingProtocol
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.ControllerPairingStage
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.BluetoothPairingRepairStage
import ptl.kitsu.app.pairing.PairingChannel
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.security.MAX_SAVED_KITSU
import ptl.kitsu.app.security.SafeLog
import ptl.kitsu.app.update.FirmwareUpdateReceipt
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
import kotlinx.coroutines.delay
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
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.encodeToJsonElement
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
) : KitsuTransport, ControllerPairingService {
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
    @Volatile private var lastGattFailureCode: String? = null
    @Volatile private var disconnectObserver: ((String) -> Unit)? = null
    @Volatile private var meshOneShotReady = false
    @Volatile private var messageProtocolVersion = 1
    @Volatile private var messageMarkReadAvailable = false
    private var failedProofs = 0
    private var proofBackoffUntilMillis = 0L

    override suspend fun connect(): ConnectResult {
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

        messageProtocolVersion = 1
        messageMarkReadAvailable = false
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
        if (credentials.pendingBondedCompanion() != null) {
            throw PairingException("pairing_finish_required")
        }
        val missing = missingPermissions()
        if (missing.isNotEmpty()) throw PairingException("bluetooth_permission_required")
        val activeJob = currentCoroutineContext()[Job]
            ?: throw PairingException("pairing_failed")
        pairingJob = activeJob
        try {
            val result = pairControllerWithPermission(label) { progress ->
                runCatching { onProgress(progress) }
            }
            runCatching {
                onProgress(ControllerPairingProgress(ControllerPairingStage.COMPLETE, "controller_paired"))
            }
            result
        } catch (failure: CancellationException) {
            withContext(NonCancellable) {
                runCatching {
                    onProgress(ControllerPairingProgress(ControllerPairingStage.CANCELLED, "pairing_cancelled"))
                }
            }
            throw PairingException("pairing_cancelled", failure)
        } finally {
            withContext(NonCancellable) { disconnect() }
            if (pairingJob === activeJob) pairingJob = null
        }
    }

    override suspend fun finishPendingPairing(
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion = pairingMutex.withLock {
        val candidate = credentials.pendingBondedCompanion()
            ?: throw PairingException("no_pending_pairing")
        val missing = missingPermissions()
        if (missing.isNotEmpty()) throw PairingException("bluetooth_permission_required")
        val activeJob = currentCoroutineContext()[Job]
            ?: throw PairingException("pairing_failed")
        pairingJob = activeJob
        try {
            runCatching {
                onProgress(
                    ControllerPairingProgress(
                        ControllerPairingStage.CONNECTING_GATT,
                        "finishing_committed_pairing",
                    ),
                )
            }
            disconnect()
            when (val result = connectWithPermission(candidate, distinguishControllerRejection = true)) {
                ConnectResult.Connected -> {
                    runCatching {
                        onProgress(
                            ControllerPairingProgress(
                                ControllerPairingStage.SAVING_CONTROLLER,
                                "saving_recovered_controller",
                            ),
                        )
                    }
                    credentials.promotePendingBondedCompanion(candidate)
                    runCatching {
                        onProgress(
                            ControllerPairingProgress(
                                ControllerPairingStage.COMPLETE,
                                "controller_paired",
                            ),
                        )
                    }
                    candidate
                }
                ConnectResult.CompanionAbsent -> throw PairingException("pairing_device_absent")
                is ConnectResult.PermissionRequired ->
                    throw PairingException("bluetooth_permission_required")
                is ConnectResult.Failed -> {
                    if (result.code == "pending_controller_rejected") {
                        credentials.savePendingBondedCompanion(null)
                        throw PairingException("pairing_not_completed_repair_required")
                    }
                    throw PairingException(result.code)
                }
            }
        } catch (failure: CancellationException) {
            throw PairingException("pairing_cancelled", failure)
        } finally {
            withContext(NonCancellable) { disconnect() }
            if (pairingJob === activeJob) pairingJob = null
        }
    }

    override suspend fun repairBluetoothPairing(
        deviceAddress: String,
        onProgress: (BluetoothPairingRepairProgress) -> Unit,
    ): BondedCompanion = pairingMutex.withLock {
        val profile = credentials.bondedCompanions().firstOrNull {
            it.deviceAddress.equals(deviceAddress, ignoreCase = true)
        } ?: throw PairingException("saved_controller_not_found")
        val missing = missingPermissions()
        if (missing.isNotEmpty()) throw PairingException(
            BluetoothPairingRepairPolicy.PERMISSION_REQUIRED,
        )
        val activeJob = currentCoroutineContext()[Job]
            ?: throw PairingException("bluetooth_pairing_repair_failed")
        pairingJob = activeJob
        try {
            runCatching {
                onProgress(
                    BluetoothPairingRepairProgress(
                        BluetoothPairingRepairStage.CHECKING_SAVED_CONTROLLER,
                        "checking_saved_controller",
                    ),
                )
            }
            disconnect()
            val repaired = repairBluetoothBondWithPermission(profile) { progress ->
                runCatching { onProgress(progress) }
            }
            val unchanged = credentials.bondedCompanions().firstOrNull {
                it.deviceAddress.equals(profile.deviceAddress, ignoreCase = true) &&
                    it.controllerIdB64 == profile.controllerIdB64 &&
                    it.controllerRootB64 == profile.controllerRootB64
            }
            if (unchanged == null || repaired != profile) {
                throw PairingException("saved_controller_changed_during_repair")
            }
            runCatching {
                onProgress(
                    BluetoothPairingRepairProgress(
                        BluetoothPairingRepairStage.BOND_COMPLETE,
                        "bluetooth_bond_repaired_controller_kept",
                    ),
                )
            }
            repaired
        } catch (failure: CancellationException) {
            withContext(NonCancellable) {
                runCatching {
                    onProgress(
                        BluetoothPairingRepairProgress(
                            BluetoothPairingRepairStage.CANCELLED,
                            "bluetooth_pairing_repair_cancelled",
                        ),
                    )
                }
            }
            throw PairingException("bluetooth_pairing_repair_cancelled", failure)
        } finally {
            // Bond repair never owns an authenticated application session. The
            // repository performs exactly one fresh GATT attempt after this newly
            // completed bond and reuses the saved controller capability there.
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
        val saved = credentials.bondedCompanions()
        // A full controller pairing always receives a fresh random controller
        // ID from Kitsu. Pairing a device address that is already saved would
        // therefore consume another firmware slot while replacing this phone's
        // only copy of the old root. Use the dedicated bond-repair path instead;
        // if the application authorization is truly gone, the owner must first
        // remove the obsolete controller explicitly on Kitsu and from this phone.
        if (saved.any { it.deviceAddress.equals(device.address, ignoreCase = true) }) {
            throw PairingException("controller_already_saved_use_repair_or_forget")
        }
        if (saved.size >= MAX_SAVED_KITSU) throw PairingException("controller_device_limit")

        report(
            ControllerPairingProgress(
                ControllerPairingStage.OS_SECURE_PAIRING,
                "compare_codes_accept_android_then_hold_prg_if_same",
            ),
        )
        val freshBond = device.bondState != BluetoothDevice.BOND_BONDED
        if (!ensureBonded(device)) throw PairingException("secure_bond_failed")

        report(ControllerPairingProgress(ControllerPairingStage.CONNECTING_GATT, "opening_secure_gatt"))
        // ACTION_BOND_STATE_CHANGED can arrive while Android is still closing its
        // SMP connection. Heltec accepts one BLE connection, so opening GATT on
        // that callback races the bonding link and returns status 22. Observing a
        // new advertisement is the release barrier; if Android still terminates
        // that first post-bond GATT, permit exactly one re-scan/reconnect.
        val connection = FreshBondGattConnector.open(
            freshBond = freshBond,
            initialDevice = device,
            awaitAdvertisement = { pairingDeviceAfterBond(device.address) },
            connect = ::openPairingGatt,
        )
        var gattDevice = connection.device
        var result = connection.result
        var retriesUsed = connection.retriesUsed
        while (true) {
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
                    val activeGatt = gatt
                        ?: throw PairingException(lastGattFailureCode ?: "gatt_disconnected")
                    val characteristic = writeCharacteristic
                        ?: throw PairingException(lastGattFailureCode ?: "gatt_disconnected")
                    if (!writeFrame(activeGatt, characteristic, encodeGattFrame(payload))) {
                        throw PairingException("gatt_write_failed")
                    }
                }

                override suspend fun receive(): ByteArray = try {
                    inbox.receive()
                } catch (failure: CancellationException) {
                    throw failure
                } catch (failure: PairingException) {
                    throw failure
                } catch (failure: Throwable) {
                    throw PairingException("gatt_disconnected", failure)
                }
            }

            var stored: BondedCompanion? = null
            var pairingPendingSeen = false
            try {
                ControllerPairingProtocol().pair(
                    label = label,
                    channel = channel,
                    persistCandidate = { grant ->
                        val displayName = runCatching { gattDevice.name }
                            .getOrNull()
                            ?.trim()
                            ?.takeIf(String::isNotEmpty)
                            ?: grant.deviceUid
                        val candidate = BondedCompanion(
                            deviceAddress = gattDevice.address,
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
                        pairingPendingSeen = true
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
                credentials.promotePendingBondedCompanion(verified)
                return verified
            } catch (failure: PairingException) {
                if (!FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
                        freshBond = freshBond,
                        retriesUsed = retriesUsed,
                        failureCode = failure.code,
                        pairingPendingSeen = pairingPendingSeen,
                        candidateStored = stored != null,
                    )
                ) throw failure
                retriesUsed += 1
                disconnect()
                gattDevice = pairingDeviceAfterBond(device.address)
                result = openPairingGatt(gattDevice)
            } finally {
                if (pairingInbox === inbox) pairingInbox = null
                inbox.close()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun pairingDeviceAfterBond(address: String): BluetoothDevice =
        when (val scan = scanForKnown(address)) {
            is DeviceScan.Found -> scan.device
            DeviceScan.Absent -> throw PairingException("pairing_device_absent")
            is DeviceScan.Failed -> throw PairingException(scan.code)
        }

    @SuppressLint("MissingPermission")
    private suspend fun openPairingGatt(device: BluetoothDevice): ConnectResult {
        disconnect()
        lastGattFailureCode = null
        val ready = CompletableDeferred<ConnectResult>()
        connectionReady = ready
        negotiatedMtu = 23
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        return withTimeoutOrNull(CONNECT_TIMEOUT_MILLIS) { ready.await() }
            ?: ConnectResult.Failed("gatt_timeout")
    }

    @SuppressLint("MissingPermission")
    private suspend fun repairBluetoothBondWithPermission(
        profile: BondedCompanion,
        report: (BluetoothPairingRepairProgress) -> Unit,
    ): BondedCompanion {
        val adapter = manager?.adapter ?: throw PairingException("bluetooth_unavailable")
        if (!adapter.isEnabled) throw PairingException("bluetooth_disabled")
        scanPrerequisiteError()?.let { throw PairingException(it) }

        val known = runCatching { adapter.getRemoteDevice(profile.deviceAddress) }
            .getOrElse { throw PairingException("saved_controller_address_invalid", it) }
        val androidStillBonded = known.bondState == BluetoothDevice.BOND_BONDED ||
            adapter.bondedDevices.any {
                it.address.equals(profile.deviceAddress, ignoreCase = true) &&
                    it.bondState == BluetoothDevice.BOND_BONDED
            }
        if (androidStillBonded) {
            // Android's supported SDK does not expose removeBond(). Do not use a
            // hidden API in a Play build: the owner must explicitly Forget this
            // system bond, while the encrypted Kitsu controller root stays saved.
            throw PairingException(BluetoothPairingRepairPolicy.ANDROID_FORGET_REQUIRED)
        }
        if (known.bondState == BluetoothDevice.BOND_BONDING) {
            throw PairingException("android_bluetooth_pairing_in_progress")
        }

        report(
            BluetoothPairingRepairProgress(
                BluetoothPairingRepairStage.SCANNING,
                "scanning_saved_kitsu_for_repair",
            ),
        )
        val device = when (val scan = scanForKnown(profile.deviceAddress)) {
            is DeviceScan.Found -> scan.device
            DeviceScan.Absent -> throw PairingException("repair_device_absent")
            is DeviceScan.Failed -> throw PairingException("repair_scan_failed")
        }
        if (device.bondState != BluetoothDevice.BOND_NONE) {
            throw PairingException(BluetoothPairingRepairPolicy.ANDROID_FORGET_REQUIRED)
        }

        report(
            BluetoothPairingRepairProgress(
                BluetoothPairingRepairStage.OS_SECURE_PAIRING,
                "accept_android_pairing_code_then_confirm_on_kitsu",
            ),
        )
        if (!ensureFreshBonded(device)) throw PairingException("secure_bond_failed")
        if (device.bondState != BluetoothDevice.BOND_BONDED) {
            throw PairingException("secure_bond_verification_failed")
        }
        return profile
    }

    @SuppressLint("MissingPermission")
    private suspend fun connectWithPermission(
        profile: BondedCompanion,
        distinguishControllerRejection: Boolean = false,
    ): ConnectResult {
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

        val seen = when (val scan = scanForKnown(bonded.address)) {
            is DeviceScan.Found -> scan.device
            DeviceScan.Absent -> return ConnectResult.CompanionAbsent
            is DeviceScan.Failed -> return ConnectResult.Failed(scan.code)
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
        val handshake = withTimeoutOrNull(CapabilityHandshake.HANDSHAKE_TIMEOUT_MILLIS) {
            runCatching {
                CapabilityHandshake().perform(controllerId, controllerRoot, ::exchangeHandshake)
            }
        }
        val session = handshake?.getOrNull()
        val handshakeCode = (handshake?.exceptionOrNull() as? HandshakeException)?.code
        controllerRoot.fill(0)
        if (session == null) {
            failedProofs++
            if (failedProofs >= MAX_FAILED_PROOFS) {
                proofBackoffUntilMillis = android.os.SystemClock.elapsedRealtime() + PROOF_BACKOFF_MILLIS
                failedProofs = 0
            }
            SafeLog.warn("ble_auth", "controller_auth_failed")
            disconnect()
            return ConnectResult.Failed(
                ControllerHandshakeFailurePolicy.code(
                    handshakeCode,
                    distinguishPendingRejection = distinguishControllerRejection,
                ),
            )
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
     * condition as an empty scan would misreport the selected Kitsu as absent.
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

    @SuppressLint("MissingPermission")
    private suspend fun ensureFreshBonded(device: BluetoothDevice): Boolean {
        // A repair retry is authorized only by a bond that this invocation starts
        // from BOND_NONE and observes reaching BOND_BONDED. An already-cached bond
        // must first be explicitly removed in Android Bluetooth settings.
        if (device.bondState != BluetoothDevice.BOND_NONE) return false
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
                if (device.bondState != BluetoothDevice.BOND_NONE) return@withContext false
                if (!device.createBond() && device.bondState !in listOf(
                        BluetoothDevice.BOND_BONDING,
                        BluetoothDevice.BOND_BONDED,
                    )
                ) return@withContext false
                if (device.bondState == BluetoothDevice.BOND_BONDED) result.complete(true)
                val completed = withTimeoutOrNull(OS_BOND_TIMEOUT_MILLIS) { result.await() } == true &&
                    device.bondState == BluetoothDevice.BOND_BONDED
                if (!completed) return@withContext false

                // ACTION_BOND_STATE_CHANGED can precede bondedDevices cache
                // convergence on some Android builds. Bound that OS propagation
                // window before allowing the repository's sole fresh GATT attempt.
                val registrationDeadline = android.os.SystemClock.elapsedRealtime() +
                    BOND_REGISTRATION_TIMEOUT_MILLIS
                do {
                    val registered = manager?.adapter?.bondedDevices?.any {
                        it.address.equals(device.address, ignoreCase = true) &&
                            it.bondState == BluetoothDevice.BOND_BONDED
                    } == true
                    if (registered) return@withContext true
                    delay(BOND_REGISTRATION_POLL_MILLIS)
                } while (android.os.SystemClock.elapsedRealtime() < registrationDeadline)
                false
            } finally {
                runCatching { context.unregisterReceiver(receiver) }
            }
        }
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (!GattCallbackBindingPolicy.accepts(this@BleKitsuTransport.gatt, gatt)) {
                runCatching { gatt.close() }
                return
            }
            val failureCode = GattStatusPolicy.connectionFailure(status)
            when {
                newState == BluetoothProfile.STATE_DISCONNECTED -> {
                    lastGattFailureCode = failureCode
                    this@BleKitsuTransport.gatt = null
                    negotiatedMtu = 23
                    writeCharacteristic = null
                    notifyCharacteristic = null
                    envelopeSession = null
                    connectedDeviceAddress = null
                    messageProtocolVersion = 1
                    messageMarkReadAvailable = false
                    connectionReady?.complete(ConnectResult.Failed(failureCode))
                    pairingInbox?.close(PairingException(failureCode))
                    failPending(failureCode)
                    disconnectObserver?.invoke(failureCode)
                    runCatching { gatt.close() }
                }
                status != BluetoothGatt.GATT_SUCCESS ->
                    connectionReady?.complete(ConnectResult.Failed(failureCode))
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
            if (!GattCallbackBindingPolicy.accepts(this@BleKitsuTransport.gatt, gatt)) return
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
            if (!GattCallbackBindingPolicy.accepts(this@BleKitsuTransport.gatt, gatt) ||
                descriptor.uuid != CLIENT_CONFIGURATION_UUID
            ) return
            connectionReady?.complete(
                if (status == BluetoothGatt.GATT_SUCCESS) ConnectResult.Connected
                else ConnectResult.Failed(GattStatusPolicy.notificationSubscriptionFailure(status)),
            )
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (!GattCallbackBindingPolicy.accepts(this@BleKitsuTransport.gatt, gatt)) return
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
            if (!GattCallbackBindingPolicy.accepts(this@BleKitsuTransport.gatt, gatt) ||
                characteristic.uuid != configuration.write
            ) return
            writeCompletion?.complete(status)
        }

        @Deprecated("Deprecated by Android")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (!GattCallbackBindingPolicy.accepts(
                    this@BleKitsuTransport.gatt, gatt, configuration.notify, characteristic.uuid,
                )
            ) return
            @Suppress("DEPRECATION")
            acceptBytes(characteristic.value ?: return)
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (!GattCallbackBindingPolicy.accepts(
                    this@BleKitsuTransport.gatt, gatt, configuration.notify, characteristic.uuid,
                )
            ) return
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
        try {
            if (verified.channel != SecureEnvelopeSession.CHANNEL_EVENT) {
                throw EnvelopeException("invalid_event_binding")
            }
            eventBus.tryEmit(FirmwareBlePayloadMapper.event(verified.operation, verified.payload))
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

    @Synchronized
    @SuppressLint("MissingPermission")
    private fun closeMalformedGatt(code: String) {
        SafeLog.warn("ble_frame", code)
        val active = gatt
        val hadActiveLink = active != null || connectedDeviceAddress != null
        gatt = null
        connectedDeviceAddress = null
        lastGattFailureCode = code
        writeCharacteristic = null
        notifyCharacteristic = null
        negotiatedMtu = 23
        envelopeSession = null
        meshOneShotReady = false
        messageProtocolVersion = 1
        messageMarkReadAvailable = false
        frameDecoder.clear()
        pairingFrameDecoder.clear()
        frameTimeoutGeneration += 1
        handshakeResponse?.completeExceptionally(EnvelopeException(code))
        handshakeResponse = null
        pairingInbox?.close(PairingException(code))
        pairingInbox = null
        runCatching { active?.disconnect() }
        runCatching { active?.close() }
        if (hadActiveLink) disconnectObserver?.invoke(code)
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
            // Do not leave temporary plaintext request JSON live after authenticated wrapping.
            payload.fill(0)
        }
        val frame = encodeGattFrame(secured)

        val response = CompletableDeferred<ByteArray>()
        pending[id] = PendingResponse(op, response)
        try {
            val started = writeFrame(activeGatt, characteristic, frame)
            if (!started) {
                // A missing/late Android write callback makes this GATT session
                // unsafe to reuse: it could otherwise complete a later request.
                closeMalformedGatt("gatt_write_failed")
                throw TransportException("gatt_write_failed")
            }
            withTimeoutOrNull(REQUEST_TIMEOUT_MILLIS) { response.await() }
                ?: run {
                    closeMalformedGatt("request_timeout")
                    throw TransportException("request_timeout")
                }
        } catch (cancelled: CancellationException) {
            // An outer action deadline may interrupt either a chunk write or the
            // receipt wait. Close the abandoned link so a late callback/receipt
            // cannot be mistaken for the next operation.
            closeMalformedGatt("request_cancelled")
            throw cancelled
        } finally {
            pending.remove(id)
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
            val status = try {
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
                if (!started) return false
                withTimeoutOrNull(WRITE_TIMEOUT_MILLIS) { completion.await() }
            } finally {
                if (writeCompletion === completion) writeCompletion = null
            }
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

    override suspend fun status(): KitsuStatus = try {
        FirmwareBlePayloadMapper.state(
            successfulPayload("state.get", buildJsonObject {}),
            java.time.Instant.now().epochSecond,
        ).also {
            meshOneShotReady = it.mesh.oneShotReady
            messageProtocolVersion = FirmwareMessageApiPolicy.protocolVersion(it.firmwareVersion)
            messageMarkReadAvailable = FirmwareMessageApiPolicy.supportsMarkRead(it.firmwareVersion)
        }
    } catch (failure: Throwable) {
        // Unknown versioned operations are fatal on older firmware. A failed/absent
        // authenticated version claim therefore always falls back to v1.
        messageProtocolVersion = 1
        messageMarkReadAvailable = false
        throw failure
    }

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

    override suspend fun messages(after: String?, limit: Int): MessagePage {
        val protocolVersion = messageProtocolVersion
        return FirmwareBlePayloadMapper.messages(
            successfulPayload(FirmwareMessageApiPolicy.operation(protocolVersion), buildJsonObject {
                put("after", after)
                put("limit", boundedLimit(limit))
            }),
            expectedProtocolVersion = protocolVersion,
        )
    }

    override suspend fun markMessagesRead(
        journalSession: String,
        messageIds: List<String>,
    ): MessageMarkReadReceipt {
        if (!messageMarkReadAvailable) throw TransportException("firmware_operation_unavailable")
        try {
            FirmwareMessageApiPolicy.requireMarkReadRequest(journalSession, messageIds)
        } catch (failure: IllegalArgumentException) {
            throw TransportException(failure.message ?: "message_read_invalid", failure)
        }
        val receipt = FirmwareBlePayloadMapper.messageMarkRead(
            successfulPayload("messages.mark_read", buildJsonObject {
                put("journal_session", journalSession)
                put("message_ids", buildJsonArray { messageIds.forEach { add(JsonPrimitive(it)) } })
            }),
        )
        if (!receipt.accepted) throw TransportException(receipt.error ?: "message_read_rejected")
        return receipt
    }

    override suspend fun channels(firmwareVersion: String?): List<MeshChannel> {
        val protocolVersion = FirmwareChannelApiPolicy.protocolVersion(firmwareVersion)
        return FirmwareBlePayloadMapper.channels(
            successfulPayload(FirmwareChannelApiPolicy.operation(protocolVersion), buildJsonObject {}),
            expectedProtocolVersion = protocolVersion,
        )
    }

    override suspend fun configureMesh(enabled: Boolean): MeshConfigurationReceipt =
        FirmwareBlePayloadMapper.meshConfiguration(
            successfulPayload("mesh.configure", buildJsonObject { put("enabled", enabled) }),
        )

    override suspend fun encounterCodes(after: String?, limit: Int): EncounterCodePage {
        if (after?.let(EncounterCodePolicy::validOpaqueCursor) == false) {
            throw TransportException("invalid_encounter_cursor")
        }
        val payload = successfulPayload(ENCOUNTER_CODES_OPERATION, buildJsonObject {
                after?.let { put("after", it) }
                put("limit", boundedLimit(limit))
            })
        return try {
            EncounterWireCodec.codePage(payload)
        } finally {
            payload.fill(0)
        }
    }

    override suspend fun nearbyKitsu(): NearbyKitsuPage = EncounterWireCodec.nearbyKitsu(
        successfulPayload(ENCOUNTER_NEIGHBORS_OPERATION, buildJsonObject {}),
    )

    override suspend fun encounterCatalog(): EncounterCatalogPage = EncounterWireCodec.catalog(
        successfulPayload(ENCOUNTER_CATALOG_OPERATION, buildJsonObject {}),
    )

    override suspend fun neighborInteraction(
        command: NeighborInteractionCommand,
    ): NeighborInteractionReceipt {
        // Expiry is evaluated by firmware, so establish trusted time before applying it.
        synchronizeClock()
        return EncounterWireCodec.neighborActionReceipt(
            successfulPayload(
                NEIGHBOR_ACTION_OPERATION,
                EncounterWireCodec.neighborActionBody(command),
            ),
            command,
        )
    }

    override suspend fun funState(): FunState = funRequest(
        FUN_STATE_GET_OPERATION,
        buildJsonObject {},
    )

    override suspend fun startExpedition(duration: ExpeditionDuration): FunState = funRequest(
        FUN_EXPEDITION_START_OPERATION,
        FunWireCodec.expeditionStartBody(duration),
    )

    override suspend fun claimExpedition(): FunState = funRequest(
        FUN_EXPEDITION_CLAIM_OPERATION,
        buildJsonObject {},
    )

    override suspend fun startStory(trigger: StoryTrigger): FunState = funRequest(
        FUN_STORY_START_OPERATION,
        FunWireCodec.storyStartBody(trigger),
    )

    override suspend fun advanceStory(storyId: Int): FunState = funRequest(
        FUN_STORY_ADVANCE_OPERATION,
        FunWireCodec.storyAdvanceBody(storyId),
    )

    override suspend fun chooseStory(storyId: Int, choice: Int): FunState = funRequest(
        FUN_STORY_CHOOSE_OPERATION,
        FunWireCodec.storyChooseBody(storyId, choice),
    )

    override suspend fun scanParty(): FunState = funRequest(
        FUN_PARTY_SCAN_OPERATION,
        buildJsonObject {},
    )

    override suspend fun hostParty(): FunState = funRequest(
        FUN_PARTY_HOST_OPERATION,
        buildJsonObject {},
    )

    override suspend fun joinParty(command: PartyJoinCommand): FunState = funRequest(
        FUN_PARTY_JOIN_OPERATION,
        FunWireCodec.partyJoinBody(command),
    )

    override suspend fun beginParty(): FunState = funRequest(
        FUN_PARTY_BEGIN_OPERATION,
        buildJsonObject {},
    )

    override suspend fun chooseParty(command: PartyRoundCommand): FunState = funRequest(
        FUN_PARTY_CHOOSE_OPERATION,
        FunWireCodec.partyChooseBody(command),
    )

    override suspend fun leaveParty(): FunState = funRequest(
        FUN_PARTY_LEAVE_OPERATION,
        buildJsonObject {},
    )

    private suspend fun funRequest(operation: String, body: JsonObject): FunState =
        FunWireCodec.state(successfulPayload(operation, body))

    override suspend fun action(command: ActionCommand): ActionReceipt {
        if (command.kind !in setOf(
                ptl.kitsu.app.model.ActionKind.PET,
                ptl.kitsu.app.model.ActionKind.FEED,
            ptl.kitsu.app.model.ActionKind.PLAY,
            ptl.kitsu.app.model.ActionKind.LISTEN_ONCE,
            ptl.kitsu.app.model.ActionKind.ADVERTISE_ONCE,
            ptl.kitsu.app.model.ActionKind.SEND_MESSAGE,
            )
        ) throw TransportException("firmware_operation_unavailable")
        val direct = DirectActionPreparer(
            synchronizeClock = ::synchronizeClock,
            currentEpochSeconds = { java.time.Instant.now().epochSecond },
            messageOneShotReady = { meshOneShotReady },
        ).prepare(command)
        return FirmwareBlePayloadMapper.action(
            successfulPayload("action.apply", json.encodeToJsonElement(direct) as JsonObject),
            command,
        )
    }

    override suspend fun forgetController(): ControllerForgetReceipt =
        FirmwareBlePayloadMapper.controllerForget(
            successfulPayload("controller.forget", buildJsonObject {}),
        )

    override fun firmwareTransferChunkBytes(): Int = if (negotiatedMtu >= 247) 4_096 else 256

    override suspend fun firmwareUpdateStatus(): FirmwareUpdateReceipt = firmwareUpdateRequest(
        "firmware.update.status",
        buildJsonObject {},
    )

    override suspend fun beginFirmwareUpdate(
        manifest: ByteArray,
        signature: ByteArray,
    ): FirmwareUpdateReceipt = firmwareUpdateRequest(
        "firmware.update.begin",
        buildJsonObject {
            put("manifest_b64", Base64.getUrlEncoder().withoutPadding().encodeToString(manifest))
            put("signature_b64", Base64.getUrlEncoder().withoutPadding().encodeToString(signature))
        },
    )

    override suspend fun writeFirmwareUpdate(
        updateId: String,
        offset: Int,
        data: ByteArray,
    ): FirmwareUpdateReceipt {
        if (!LOWER_SHA256.matches(updateId) || offset < 0 || data.isEmpty() || data.size > 4_096) {
            throw TransportException("invalid_firmware_update_chunk")
        }
        return firmwareUpdateRequest(
            "firmware.update.write",
            buildJsonObject {
                put("update_id", updateId)
                put("offset", offset)
                put("data_b64", Base64.getUrlEncoder().withoutPadding().encodeToString(data))
            },
        )
    }

    override suspend fun finishFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
        firmwareUpdateIdRequest("firmware.update.finish", updateId)

    override suspend fun rebootFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
        firmwareUpdateIdRequest("firmware.update.reboot", updateId)

    override suspend fun abortFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
        firmwareUpdateIdRequest("firmware.update.abort", updateId)

    private suspend fun firmwareUpdateIdRequest(operation: String, updateId: String): FirmwareUpdateReceipt {
        if (!LOWER_SHA256.matches(updateId)) throw TransportException("invalid_update_id")
        return firmwareUpdateRequest(operation, buildJsonObject { put("update_id", updateId) })
    }

    private suspend fun firmwareUpdateRequest(
        operation: String,
        body: JsonObject,
    ): FirmwareUpdateReceipt = FirmwareBlePayloadMapper.firmwareUpdate(request(operation, body))

    override fun events(after: String?): Flow<EventEnvelope> = flow { emitAll(eventBus) }

    override fun isConnectedTo(deviceAddress: String): Boolean =
        connectedDeviceAddress?.equals(deviceAddress, ignoreCase = true) == true &&
            gatt != null && writeCharacteristic != null && notifyCharacteristic != null &&
            envelopeSession != null

    internal fun setDisconnectObserver(observer: (String) -> Unit) {
        disconnectObserver = observer
    }

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
        lastGattFailureCode = null
        meshOneShotReady = false
        messageProtocolVersion = 1
        messageMarkReadAvailable = false
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

    override suspend fun synchronizeClock() {
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
        private val LOWER_SHA256 = Regex("^[0-9a-f]{64}$")
        private val CLIENT_CONFIGURATION_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val CONNECT_TIMEOUT_MILLIS = 12_000L
        private const val OS_BOND_TIMEOUT_MILLIS = 90_000L
        private const val BOND_REGISTRATION_TIMEOUT_MILLIS = 2_000L
        private const val BOND_REGISTRATION_POLL_MILLIS = 50L
        // A valid authenticated response may carry up to 12 KiB and therefore
        // hundreds of 20-byte notification fragments when Android negotiates
        // the minimum MTU. Ten seconds falsely classified slow mesh snapshots
        // as a broken session and deliberately tore down GATT.
        private const val REQUEST_TIMEOUT_MILLIS = 30_000L
        private const val WRITE_TIMEOUT_MILLIS = 5_000L
        private const val PAIRING_INBOX_CAPACITY = 4
        private const val MAX_FAILED_PROOFS = 3
        private const val PROOF_BACKOFF_MILLIS = 30_000L
    }
}
