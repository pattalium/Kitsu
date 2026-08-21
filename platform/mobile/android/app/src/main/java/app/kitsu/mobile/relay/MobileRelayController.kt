package app.kitsu.mobile.relay

import android.content.Context
import android.content.Intent
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
)

/**
 * Owns the opt-in preference and the deliberately small foreground relay loop.
 * Enrollment documents and signed frames remain byte-exact and memory-only.
 */
class MobileRelayController(
    private val context: Context,
    private val credentials: CredentialStore,
    private val backend: MobileRelayBackend,
    private val sessions: MobileRelayDeviceSessionFactory,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val settingsMutex = Mutex()
    private val initialized = CompletableDeferred<Unit>()
    private val mutableState = MutableStateFlow(MobileRelayUiState())
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }
    private val random = SecureRandom()
    private val nextSpoolRecordId = AtomicLong(System.currentTimeMillis().coerceAtLeast(1L))

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

    suspend fun setEnabled(enabled: Boolean) = settingsMutex.withLock {
        val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
        if (enabled && bonds.isEmpty()) throw TransportException("mobile_relay_pairing_required")
        val current = loadOrCreateSettings()
        val selected = if (enabled) {
            bonds.map(BondedCompanion::deviceAddress)
        } else {
            current.selectedDeviceAddresses.filter { address ->
                bonds.any { it.deviceAddress.equals(address, ignoreCase = true) }
            }
        }
        val updated = current.copy(enabled = enabled, selectedDeviceAddresses = selected)
        credentials.saveMobileRelaySettings(updated)
        publish(updated, bonds, running = enabled && mutableState.value.running)
        if (enabled) startService() else context.stopService(serviceIntent())
    }

    suspend fun refreshDevices() = settingsMutex.withLock {
        val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
        val current = loadOrCreateSettings()
        val updated = if (current.enabled) {
            current.copy(selectedDeviceAddresses = bonds.map(BondedCompanion::deviceAddress))
        } else {
            current
        }
        if (updated != current) credentials.saveMobileRelaySettings(updated)
        publish(updated, bonds, running = mutableState.value.running)
    }

    suspend fun runForegroundService() {
        mutableState.value = mutableState.value.copy(
            running = true,
            detail = "connecting_public_gateway",
            enrollmentRemainingMillis = null,
        )
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
                    mutableState.value = mutableState.value.copy(
                        running = true,
                        detail = (failure as? TransportException)?.code ?: "retrying",
                        enrollmentRemainingMillis = null,
                    )
                    delay(RETRY_MILLIS)
                }
            }
        } finally {
            mutableState.value = mutableState.value.copy(
                running = false,
                detail = "off",
                enrollmentRemainingMillis = null,
            )
        }
    }

    private suspend fun initialize() = settingsMutex.withLock {
        try {
            val bonds = credentials.bondedCompanions().take(MAX_MOBILE_RELAY_DEVICES)
            val settings = loadOrCreateSettings()
            validateSettings(settings, bonds)
            publish(settings, bonds, running = false)
            if (settings.enabled) startService()
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
        val websocket = launch {
            while (isActive) {
                try {
                    backend.downlinks(initialSettings.installationId).collect { exactFrame ->
                        inbound.send(exactFrame)
                    }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Throwable) {
                    SafeLog.warn("mobile_relay", "downlink_retry", failure)
                    delay(RETRY_MILLIS)
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
                var completed = 0
                var lastFailureCode: String? = null
                for (bond in bonds) {
                    val session = sessions.create(bond)
                    try {
                        when (val connected = session.connect()) {
                            ConnectResult.Connected -> Unit
                            ConnectResult.CompanionAbsent -> throw TransportException("companion_absent")
                            is ConnectResult.PermissionRequired ->
                                throw TransportException("bluetooth_permission_required")
                            is ConnectResult.Failed -> throw TransportException(connected.code)
                        }
                        verifyConfigureReceipt(
                            session.configureRelay(identity.gatewayId, identity.caCertificateDerB64),
                        )
                        // Configuration may invalidate credentials bound to a different gateway.
                        val status = session.status()
                        val companionId = if (status.lan.gatewayEnrolled == true) {
                            val binding = settings.companionBindings
                                .singleOrNull { it.hardwareUid == status.deviceId }
                                ?: throw TransportException("mobile_relay_companion_binding_missing")
                            clearPendingEnrollment(settings.installationId, status.deviceId)
                            binding.companionId
                        } else {
                            enroll(session, status.deviceId, status.displayName, settings, identity)
                        }
                        relayUplink(session, initialSettings.installationId)
                        deliverDownlinks(session, companionId, pending, deliveredActionIds)
                        completed += 1
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Throwable) {
                        SafeLog.warn("mobile_relay", "device_retry", failure)
                        val code = (failure as? TransportException)?.code ?: "retrying_devices"
                        if (lastFailureCode != "existing_gateway_enrollment_requires_reset") {
                            lastFailureCode = code
                        }
                    } finally {
                        runCatching { session.disconnect() }
                    }
                }
                mutableState.value = mutableState.value.copy(
                    running = true,
                    detail = if (completed == bonds.size) {
                        "connected_public_gateway"
                    } else {
                        lastFailureCode ?: "retrying_devices"
                    },
                    enrollmentRemainingMillis = null,
                )
                delay(DEVICE_POLL_MILLIS)
            }
        } finally {
            websocket.cancel()
            inbound.close()
            pending.forEach { it.exactFrame.fill(0) }
        }
    }

    private suspend fun enroll(
        session: MobileRelayDeviceSession,
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
                throw TransportException("existing_gateway_enrollment_requires_reset")
            }
            mutableState.value = mutableState.value.copy(
                running = true,
                detail = "finishing_public_gateway",
                enrollmentRemainingMillis = null,
            )
            return completeEnrollment(
                session,
                hardwareUid,
                settings,
                identity,
                pending.enrollmentId,
                recovering = true,
            )
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
            mutableState.value = mutableState.value.copy(
                running = true,
                detail = "hold_prg_to_connect",
                enrollmentRemainingMillis = remainingMillis,
            )
            delay(ENROLLMENT_POLL_MILLIS)
            finish = session.finishEnrollment(enrollmentId)
            validateEnrollmentReceipt(finish, enrollmentId, allowPending = true)
            if (finish.accepted && finish.state == "ready_for_wifi") break
        }
        if (finish?.accepted != true || finish.state != "ready_for_wifi") {
            throw TransportException("mobile_relay_enrollment_expired")
        }
        mutableState.value = mutableState.value.copy(
            running = true,
            detail = "finishing_public_gateway",
            enrollmentRemainingMillis = null,
        )
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
        val exactRequest = MobileRelayTransfer.pull(MobileRelayPullKind.ENROLLMENT, session::pull)
            ?: throw TransportException(
                if (recovering) {
                    "existing_gateway_enrollment_requires_reset"
                } else {
                    "mobile_relay_enrollment_request_missing"
                },
            )
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

    private fun validateSettings(settings: MobileRelaySettings, bonds: List<BondedCompanion>) {
        if (!MobileRelayWirePolicy.canonicalUuid(settings.installationId) ||
            !MobileRelayWirePolicy.canonicalRelayCredential(settings.relayCredentialB64) ||
            settings.selectedDeviceAddresses.size > MAX_MOBILE_RELAY_DEVICES ||
            settings.selectedDeviceAddresses.distinctBy(String::lowercase).size !=
            settings.selectedDeviceAddresses.size ||
            settings.selectedDeviceAddresses.any { selected ->
                bonds.none { it.deviceAddress.equals(selected, ignoreCase = true) }
            } || settings.companionBindings.size > MAX_MOBILE_RELAY_DEVICES ||
            settings.companionBindings.distinctBy(MobileRelayCompanionBinding::hardwareUid).size !=
            settings.companionBindings.size ||
            settings.companionBindings.distinctBy(MobileRelayCompanionBinding::companionId).size !=
            settings.companionBindings.size ||
            settings.companionBindings.any {
                !HARDWARE_UID.matches(it.hardwareUid) ||
                    !MobileRelayWirePolicy.canonicalUuid(it.companionId)
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
        running: Boolean,
    ) {
        mutableState.value = MobileRelayUiState(
            enabled = settings.enabled,
            running = running,
            pairedDeviceCount = bonds.size.coerceAtMost(MAX_MOBILE_RELAY_DEVICES),
            selectedDeviceCount = settings.selectedDeviceAddresses.size,
            detail = if (settings.enabled) {
                if (running) mutableState.value.detail else "connecting_public_gateway"
            } else {
                "off"
            },
            enrollmentRemainingMillis = if (running) {
                mutableState.value.enrollmentRemainingMillis
            } else {
                null
            },
        )
    }

    private suspend fun saveCompanionBinding(
        installationId: String,
        hardwareUid: String,
        companionId: String,
    ) = settingsMutex.withLock {
        val current = loadOrCreateSettings()
        if (current.installationId != installationId) {
            throw TransportException("mobile_relay_binding_changed")
        }
        val binding = MobileRelayCompanionBinding(hardwareUid, companionId)
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

    private fun newRelayCredential(): String = Base64.getUrlEncoder().withoutPadding()
        .encodeToString(ByteArray(MobileRelayWirePolicy.RELAY_CREDENTIAL_BYTES).also(random::nextBytes))

    private fun startService() {
        runCatching { ContextCompat.startForegroundService(context, serviceIntent()) }
            .onFailure {
                SafeLog.warn("mobile_relay", "foreground_start_failed", it)
                mutableState.value = mutableState.value.copy(detail = "foreground_start_failed")
            }
    }

    private fun serviceIntent() = Intent(context, MobileRelayForegroundService::class.java)

    private fun verifyConfigureReceipt(receipt: MobileRelayReceipt) {
        if (receipt.schema != MobileRelayWirePolicy.RECEIPT_SCHEMA ||
            receipt.kind != "configure" || !receipt.accepted || receipt.nextOffset != 0 ||
            !receipt.complete || receipt.errorCode != null
        ) throw TransportException(receipt.errorCode ?: "mobile_relay_configure_rejected")
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

        private const val RETRY_MILLIS = 2_000L
        private const val DEVICE_POLL_MILLIS = 2_000L
        private const val ENROLLMENT_POLL_MILLIS = 1_000L
        private const val DEFAULT_ENROLLMENT_WINDOW_MILLIS = 60_000
        private const val MAX_ENROLLMENT_WINDOW_MILLIS = 300_000
        private const val MAX_PENDING_DOWNLINKS = 32
        private const val MAX_DELIVERED_ACTION_IDS = 64
        private const val MAX_UPLINKS_PER_DEVICE_CYCLE = 4
        private val HARDWARE_UID = Regex("^KT[0-9A-F]{4}$")
    }
}
