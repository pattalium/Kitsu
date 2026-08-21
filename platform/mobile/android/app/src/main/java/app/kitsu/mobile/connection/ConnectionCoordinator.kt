package app.kitsu.mobile.connection

import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.KitsuTransport
import app.kitsu.mobile.transport.RemoteSnapshotPolicy
import app.kitsu.mobile.transport.TransportException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

data class ConnectionState(
    val mode: ConnectionMode = ConnectionMode.OFFLINE,
    val connected: Boolean = false,
    val detail: String = "not_connected",
    val explicitRemoteAttempt: Boolean = false,
)

class ConnectionCoordinator internal constructor(
    private val direct: KitsuTransport,
    private val backend: KitsuTransport,
    private val reconnectSuppressionStore: ReconnectSuppressionStore =
        InMemoryReconnectSuppressionStore(),
) {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(ConnectionState())
    val state: StateFlow<ConnectionState> = mutableState.asStateFlow()

    @Volatile private var active: KitsuTransport? = null
    // Loaded synchronously during service construction, before any ViewModel init can request
    // an automatic connection. Store read failures fail closed instead of surprising the owner.
    @Volatile private var automaticReconnectSuppressed = runCatching {
        reconnectSuppressionStore.readSuppressed()
    }.getOrDefault(true)
    @Volatile private var backendPollingAuthorizedUntilNanos = 0L

    suspend fun connect(userInitiated: Boolean = false): ConnectionState = mutex.withLock {
        if (!userInitiated && automaticReconnectSuppressed) {
            return@withLock offline("user_disconnected")
        }
        if (userInitiated) {
            // This is the only production path that clears the durable owner choice.
            automaticReconnectSuppressed = false
            reconnectSuppressionStore.writeSuppressed(false)
        }
        backendPollingAuthorizedUntilNanos = 0L
        disconnectTransports()
        mutableState.value = ConnectionState(ConnectionMode.CONNECTING, false, "scanning_bonded_kitsu")
        try {
            when (val directResult = direct.connect()) {
                ConnectResult.Connected -> activate(direct, "bonded_kitsu_reachable")
                ConnectResult.CompanionAbsent -> {
                    backendPollingAuthorizedUntilNanos = System.nanoTime() + BACKEND_POLL_AUTHORIZATION_NANOS
                    mutableState.value = ConnectionState(
                        ConnectionMode.CONNECTING,
                        false,
                        "bonded_kitsu_absent_checking_backend",
                    )
                    when (val remoteResult = backend.connect()) {
                        ConnectResult.Connected -> activate(backend, "bonded_kitsu_absent")
                        is ConnectResult.PermissionRequired -> offline("backend_permission_invalid")
                        is ConnectResult.Failed -> offline(remoteResult.code)
                        ConnectResult.CompanionAbsent -> offline("backend_unavailable")
                    }
                }
                is ConnectResult.PermissionRequired -> {
                    mutableState.value = ConnectionState(
                        ConnectionMode.PERMISSION_REQUIRED,
                        false,
                        directResult.permissions.joinToString(","),
                    )
                    mutableState.value
                }
                is ConnectResult.Failed -> {
                    // A present/bonded direct path failed. Do not silently gain a remote control path.
                    offline(directResult.code)
                }
            }
        } catch (cancelled: CancellationException) {
            disconnectTransports()
            offline(if (automaticReconnectSuppressed) "user_disconnected" else "connection_cancelled")
            throw cancelled
        } catch (_: Throwable) {
            disconnectTransports()
            offline("connection_failed")
        }
    }

    /** Explicit nearby-only connection. Unlike the automatic policy, an
     * absent Bluetooth device remains offline and never opens a remote path. */
    suspend fun connectDirect(userInitiated: Boolean = true): ConnectionState = mutex.withLock {
        if (!userInitiated && automaticReconnectSuppressed) {
            return@withLock offline("user_disconnected")
        }
        if (userInitiated) {
            automaticReconnectSuppressed = false
            reconnectSuppressionStore.writeSuppressed(false)
        }
        backendPollingAuthorizedUntilNanos = 0L
        disconnectTransports()
        mutableState.value = ConnectionState(ConnectionMode.CONNECTING, false, "scanning_bonded_kitsu")
        try {
            when (val result = direct.connect()) {
                ConnectResult.Connected -> activate(direct, "bonded_kitsu_reachable")
                ConnectResult.CompanionAbsent -> offline("bonded_kitsu_absent")
                is ConnectResult.PermissionRequired -> {
                    mutableState.value = ConnectionState(
                        ConnectionMode.PERMISSION_REQUIRED,
                        false,
                        result.permissions.joinToString(","),
                    )
                    mutableState.value
                }
                is ConnectResult.Failed -> offline(result.code)
            }
        } catch (cancelled: CancellationException) {
            disconnectTransports()
            offline(if (automaticReconnectSuppressed) "user_disconnected" else "connection_cancelled")
            throw cancelled
        } catch (_: Throwable) {
            disconnectTransports()
            offline("direct_connect_failed")
        }
    }

    /**
     * Explicitly selects the authenticated owner-service path. Automatic
     * connection continues to prefer BLE and requires a confirmed absent scan
     * before remote fallback; this method exists so an owner can intentionally
     * end a nearby BLE session and let Kitsu's Wi-Fi runtime take priority.
     */
    suspend fun connectRemote(userInitiated: Boolean = true): ConnectionState = mutex.withLock {
        if (!userInitiated && automaticReconnectSuppressed) {
            return@withLock offline("user_disconnected")
        }
        if (userInitiated) {
            automaticReconnectSuppressed = false
            reconnectSuppressionStore.writeSuppressed(false)
        }
        backendPollingAuthorizedUntilNanos = 0L
        disconnectTransports()
        mutableState.value = ConnectionState(
            ConnectionMode.CONNECTING,
            false,
            "checking_authenticated_remote_service",
            explicitRemoteAttempt = true,
        )
        try {
            when (val result = backend.connect()) {
                ConnectResult.Connected -> activate(backend, "owner_selected_remote_service")
                is ConnectResult.PermissionRequired ->
                    offline("backend_permission_invalid", explicitRemoteAttempt = true)
                is ConnectResult.Failed -> offline(result.code, explicitRemoteAttempt = true)
                ConnectResult.CompanionAbsent ->
                    offline("backend_unavailable", explicitRemoteAttempt = true)
            }
        } catch (cancelled: CancellationException) {
            disconnectTransports()
            offline(
                if (automaticReconnectSuppressed) "user_disconnected" else "connection_cancelled",
                explicitRemoteAttempt = true,
            )
            throw cancelled
        } catch (_: Throwable) {
            runCatching { backend.disconnect() }
            offline("backend_unavailable", explicitRemoteAttempt = true)
        }
    }

    suspend fun disconnect(suppressAutomaticReconnect: Boolean = false) = mutex.withLock {
        if (suppressAutomaticReconnect) recordUserDisconnectIntent()
        backendPollingAuthorizedUntilNanos = 0L
        disconnectTransports()
        mutableState.value = ConnectionState(
            ConnectionMode.OFFLINE,
            false,
            if (automaticReconnectSuppressed) "user_disconnected" else "disconnected",
        )
    }

    /**
     * Persists before asynchronous teardown starts so process death immediately after a tap cannot
     * turn the next cold launch into an automatic Bluetooth/backend connection.
     */
    fun recordUserDisconnectIntent(): Boolean {
        automaticReconnectSuppressed = true
        return runCatching { reconnectSuppressionStore.writeSuppressed(true) }.getOrDefault(false)
    }

    suspend fun <T> withTransport(block: suspend (KitsuTransport) -> T): T {
        val transport = active ?: throw TransportException("not_connected")
        return block(transport)
    }

    fun isDirect(): Boolean = active?.mode == ConnectionMode.DIRECT_BLE

    fun isAutomaticReconnectSuppressed(): Boolean = automaticReconnectSuppressed

    /**
     * Enrollment may take a few seconds to appear in the owner projection.
     * This retries only the backend and only after the immediately preceding
     * BLE scan proved that the bonded Kitsu was absent.
     */
    suspend fun pollBackendAfterConfirmedAbsence(): ConnectionState = mutex.withLock {
        if (automaticReconnectSuppressed || System.nanoTime() >= backendPollingAuthorizedUntilNanos) {
            return@withLock offline("backend_poll_not_authorized")
        }
        active = null
        runCatching { backend.disconnect() }
        mutableState.value = ConnectionState(
            ConnectionMode.CONNECTING,
            false,
            "enrollment_waiting_for_backend",
        )
        when (val result = backend.connect()) {
            ConnectResult.Connected -> {
                backendPollingAuthorizedUntilNanos = 0L
                activate(backend, "enrollment_completed")
            }
            is ConnectResult.Failed -> offline(result.code)
            is ConnectResult.PermissionRequired -> offline("backend_permission_invalid")
            ConnectResult.CompanionAbsent -> offline("backend_unavailable")
        }
    }

    /**
     * Starts the explicit BLE-to-Wi-Fi enrollment handoff. This is the only
     * path that may authorize backend-only polling without another BLE scan:
     * it can be entered only from an active direct session after the device
     * returned its bound ready_for_wifi enrollment receipt.
     */
    suspend fun beginEnrollmentRemoteHandoff(): ConnectionState = mutex.withLock {
        if (automaticReconnectSuppressed) throw TransportException("user_disconnected")
        if (active?.mode != ConnectionMode.DIRECT_BLE) {
            throw TransportException("direct_ble_required")
        }
        backendPollingAuthorizedUntilNanos =
            System.nanoTime() + ENROLLMENT_BACKEND_POLL_AUTHORIZATION_NANOS
        disconnectTransports()
        offline("enrollment_switching_to_wifi")
    }

    /**
     * Polls the backend only, and activates it only for the expected device
     * through the expected authenticated gateway. A generic connected result,
     * a BLE reconnection, or an unauthenticated/stale projection can never
     * complete enrollment.
     */
    suspend fun pollEnrollmentBackend(
        expectedDeviceId: String,
        expectedGatewayId: String,
    ): ConnectionState = mutex.withLock {
        if (automaticReconnectSuppressed || System.nanoTime() >= backendPollingAuthorizedUntilNanos) {
            return@withLock offline("backend_poll_not_authorized")
        }
        active = null
        runCatching { backend.disconnect() }
        mutableState.value = ConnectionState(
            ConnectionMode.CONNECTING,
            false,
            "enrollment_waiting_for_authenticated_backend",
        )
        try {
            when (val result = backend.connect()) {
                ConnectResult.Connected -> {
                    val status = backend.status()
                    val error = RemoteSnapshotPolicy.validationError(
                        status,
                        expectedDeviceId = expectedDeviceId,
                        expectedGatewayId = expectedGatewayId,
                    )
                    if (error != null) {
                        runCatching { backend.disconnect() }
                        offline(error)
                    } else {
                        backendPollingAuthorizedUntilNanos = 0L
                        activate(backend, "enrollment_authenticated_remote_path")
                    }
                }
                is ConnectResult.Failed -> offline(result.code)
                is ConnectResult.PermissionRequired -> offline("backend_permission_invalid")
                ConnectResult.CompanionAbsent -> offline("backend_unavailable")
            }
        } catch (cancelled: CancellationException) {
            runCatching { backend.disconnect() }
            throw cancelled
        } catch (failure: TransportException) {
            runCatching { backend.disconnect() }
            offline(failure.code)
        } catch (_: Throwable) {
            runCatching { backend.disconnect() }
            offline("backend_unavailable")
        }
    }

    private suspend fun disconnectTransports() {
        val transports = listOfNotNull(active, direct, backend).distinct()
        active = null
        transports.forEach { transport ->
            try {
                transport.disconnect()
            } catch (_: Throwable) {
                // Continue tearing down the other path. Disconnect is best-effort and must
                // never leave an alternate remote or BLE session alive.
            }
        }
    }

    private fun activate(transport: KitsuTransport, detail: String): ConnectionState {
        active = transport
        return ConnectionState(transport.mode, true, detail).also { mutableState.value = it }
    }

    private fun offline(
        detail: String,
        explicitRemoteAttempt: Boolean = false,
    ): ConnectionState = ConnectionState(
        ConnectionMode.OFFLINE,
        false,
        detail,
        explicitRemoteAttempt,
    ).also { mutableState.value = it }

    private companion object {
        const val BACKEND_POLL_AUTHORIZATION_NANOS = 90_000_000_000L
        const val ENROLLMENT_BACKEND_POLL_AUTHORIZATION_NANOS = 120_000_000_000L
    }
}
