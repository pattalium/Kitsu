package ptl.kitsu.app.connection

import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.ClockSyncFailurePolicy
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.TransportException
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
    val warning: String? = null,
)

/** Owns the app's single authenticated GATT session. There is no network fallback. */
class ConnectionCoordinator internal constructor(
    private val direct: KitsuTransport,
    private val reconnectSuppressionStore: ReconnectSuppressionStore =
        InMemoryReconnectSuppressionStore(),
) {
    private val mutex = Mutex()
    private val connectionStateGuard = Any()
    private val mutableState = MutableStateFlow(ConnectionState())
    val state: StateFlow<ConnectionState> = mutableState.asStateFlow()

    @Volatile private var active = false
    private var connectionAttemptSequence = 0L
    private var connectingAttempt: Long? = null
    private var connectingDisconnectDetail: String? = null
    @Volatile private var automaticReconnectSuppressed = runCatching {
        reconnectSuppressionStore.readSuppressed()
    }.getOrDefault(true)

    suspend fun connect(userInitiated: Boolean = false): ConnectionState = mutex.withLock {
        if (!userInitiated && automaticReconnectSuppressed) return@withLock offline("user_disconnected")
        if (userInitiated) {
            automaticReconnectSuppressed = false
            reconnectSuppressionStore.writeSuppressed(false)
        }
        synchronized(connectionStateGuard) {
            active = false
            connectingAttempt = null
            connectingDisconnectDetail = null
        }
        runCatching { direct.disconnect() }
        val attempt = beginConnectionAttempt()
        try {
            val result = direct.connect()
            val warning = if (result == ConnectResult.Connected) direct.connectionWarning() else null
            completeConnectionAttempt(attempt, result, warning)
        } catch (cancelled: CancellationException) {
            runCatching { direct.disconnect() }
            failConnectionAttempt(
                attempt,
                if (automaticReconnectSuppressed) "user_disconnected" else "connection_cancelled",
            )
            throw cancelled
        } catch (_: Throwable) {
            runCatching { direct.disconnect() }
            failConnectionAttempt(attempt, "direct_connect_failed")
        }
    }

    suspend fun disconnect(suppressAutomaticReconnect: Boolean = false) = mutex.withLock {
        if (suppressAutomaticReconnect) recordUserDisconnectIntent()
        synchronized(connectionStateGuard) {
            active = false
            connectingAttempt = null
            connectingDisconnectDetail = null
        }
        runCatching { direct.disconnect() }
        offline(if (automaticReconnectSuppressed) "user_disconnected" else "disconnected")
    }

    fun recordUserDisconnectIntent(): Boolean {
        automaticReconnectSuppressed = true
        return runCatching { reconnectSuppressionStore.writeSuppressed(true) }.getOrDefault(false)
    }

    suspend fun <T> withTransport(block: suspend (KitsuTransport) -> T): T {
        if (!active) throw TransportException("not_connected")
        return try {
            block(direct).also { publishTransportWarning() }
        } catch (failure: Throwable) {
            publishTransportWarning()
            throw failure
        }
    }

    suspend fun synchronizeClock() {
        try {
            withTransport { it.synchronizeClock() }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            if (active && direct.connectionWarning() == null) {
                publishWarningIfConnected(ClockSyncFailurePolicy.code(failure))
            }
            throw failure
        }
    }

    fun isDirect(): Boolean = active && direct.mode == ConnectionMode.DIRECT_BLE
    fun isConnectedTo(deviceAddress: String): Boolean = active && direct.isConnectedTo(deviceAddress)
    fun isAutomaticReconnectSuppressed(): Boolean = automaticReconnectSuppressed

    /** Called by the direct GATT owner when Android reports an unsolicited link loss. */
    internal fun onDirectTransportDisconnected(detail: String) {
        synchronized(connectionStateGuard) {
            if (connectingAttempt != null) {
                val boundDetail = connectingDisconnectDetail ?: detail
                connectingDisconnectDetail = boundDetail
                active = false
                mutableState.value = ConnectionState(ConnectionMode.OFFLINE, false, boundDetail)
                return
            }
            if (!active) return
            active = false
            mutableState.value = ConnectionState(ConnectionMode.OFFLINE, false, detail)
        }
    }

    private fun beginConnectionAttempt(): Long = synchronized(connectionStateGuard) {
        connectionAttemptSequence += 1L
        connectionAttemptSequence.also { attempt ->
            connectingAttempt = attempt
            connectingDisconnectDetail = null
            active = false
            mutableState.value = ConnectionState(
                ConnectionMode.CONNECTING,
                false,
                "scanning_selected_kitsu",
            )
        }
    }

    private fun completeConnectionAttempt(
        attempt: Long,
        result: ConnectResult,
        warning: String?,
    ): ConnectionState = synchronized(connectionStateGuard) {
        if (connectingAttempt != attempt) return@synchronized mutableState.value
        val disconnectDetail = connectingDisconnectDetail
        connectingAttempt = null
        connectingDisconnectDetail = null
        val completed = when {
            disconnectDetail != null -> ConnectionState(ConnectionMode.OFFLINE, false, disconnectDetail)
            result == ConnectResult.Connected -> ConnectionState(
                ConnectionMode.DIRECT_BLE,
                true,
                "selected_kitsu_reachable",
                warning,
            )
            result == ConnectResult.CompanionAbsent -> ConnectionState(
                ConnectionMode.OFFLINE,
                false,
                "selected_kitsu_absent",
            )
            result is ConnectResult.PermissionRequired -> ConnectionState(
                ConnectionMode.PERMISSION_REQUIRED,
                false,
                result.permissions.joinToString(","),
            )
            result is ConnectResult.Failed -> ConnectionState(ConnectionMode.OFFLINE, false, result.code)
            else -> ConnectionState(ConnectionMode.OFFLINE, false, "direct_connect_failed")
        }
        active = completed.connected
        mutableState.value = completed
        completed
    }

    private fun failConnectionAttempt(attempt: Long, fallback: String): ConnectionState =
        completeConnectionAttempt(attempt, ConnectResult.Failed(fallback), warning = null)

    private fun publishTransportWarning() {
        publishWarningIfConnected(direct.connectionWarning())
    }

    private fun publishWarningIfConnected(warning: String?) {
        while (active) {
            val current = mutableState.value
            if (!current.connected || current.mode != ConnectionMode.DIRECT_BLE) return
            if (current.warning == warning) return
            // A callback may publish OFFLINE concurrently. CAS prevents this older
            // connected snapshot from resurrecting the closed session.
            if (active && mutableState.compareAndSet(current, current.copy(warning = warning))) return
        }
    }

    private fun offline(detail: String): ConnectionState = synchronized(connectionStateGuard) {
        active = false
        connectingAttempt = null
        connectingDisconnectDetail = null
        ConnectionState(ConnectionMode.OFFLINE, false, detail).also { mutableState.value = it }
    }
}
