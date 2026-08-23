package ptl.kitsu.app.connection

import ptl.kitsu.app.transport.ConnectResult
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
)

/** Owns the app's single authenticated GATT session. There is no network fallback. */
class ConnectionCoordinator internal constructor(
    private val direct: KitsuTransport,
    private val reconnectSuppressionStore: ReconnectSuppressionStore =
        InMemoryReconnectSuppressionStore(),
) {
    private val mutex = Mutex()
    private val mutableState = MutableStateFlow(ConnectionState())
    val state: StateFlow<ConnectionState> = mutableState.asStateFlow()

    @Volatile private var active = false
    @Volatile private var automaticReconnectSuppressed = runCatching {
        reconnectSuppressionStore.readSuppressed()
    }.getOrDefault(true)

    suspend fun connect(userInitiated: Boolean = false): ConnectionState = mutex.withLock {
        if (!userInitiated && automaticReconnectSuppressed) return@withLock offline("user_disconnected")
        if (userInitiated) {
            automaticReconnectSuppressed = false
            reconnectSuppressionStore.writeSuppressed(false)
        }
        active = false
        runCatching { direct.disconnect() }
        mutableState.value = ConnectionState(ConnectionMode.CONNECTING, false, "scanning_selected_kitsu")
        try {
            when (val result = direct.connect()) {
                ConnectResult.Connected -> activate("selected_kitsu_reachable")
                ConnectResult.CompanionAbsent -> offline("selected_kitsu_absent")
                is ConnectResult.PermissionRequired -> ConnectionState(
                    ConnectionMode.PERMISSION_REQUIRED,
                    false,
                    result.permissions.joinToString(","),
                ).also { mutableState.value = it }
                is ConnectResult.Failed -> offline(result.code)
            }
        } catch (cancelled: CancellationException) {
            active = false
            runCatching { direct.disconnect() }
            offline(if (automaticReconnectSuppressed) "user_disconnected" else "connection_cancelled")
            throw cancelled
        } catch (_: Throwable) {
            active = false
            runCatching { direct.disconnect() }
            offline("direct_connect_failed")
        }
    }

    suspend fun disconnect(suppressAutomaticReconnect: Boolean = false) = mutex.withLock {
        if (suppressAutomaticReconnect) recordUserDisconnectIntent()
        active = false
        runCatching { direct.disconnect() }
        mutableState.value = ConnectionState(
            ConnectionMode.OFFLINE,
            false,
            if (automaticReconnectSuppressed) "user_disconnected" else "disconnected",
        )
    }

    fun recordUserDisconnectIntent(): Boolean {
        automaticReconnectSuppressed = true
        return runCatching { reconnectSuppressionStore.writeSuppressed(true) }.getOrDefault(false)
    }

    suspend fun <T> withTransport(block: suspend (KitsuTransport) -> T): T {
        if (!active) throw TransportException("not_connected")
        return block(direct)
    }

    suspend fun synchronizeClock() = withTransport { it.synchronizeClock() }

    fun isDirect(): Boolean = active && direct.mode == ConnectionMode.DIRECT_BLE
    fun isConnectedTo(deviceAddress: String): Boolean = active && direct.isConnectedTo(deviceAddress)
    fun isAutomaticReconnectSuppressed(): Boolean = automaticReconnectSuppressed

    /** Called by the direct GATT owner when Android reports an unsolicited link loss. */
    internal fun onDirectTransportDisconnected(detail: String) {
        if (!active) return
        active = false
        mutableState.value = ConnectionState(ConnectionMode.OFFLINE, false, detail)
    }

    private fun activate(detail: String): ConnectionState {
        active = true
        return ConnectionState(ConnectionMode.DIRECT_BLE, true, detail).also { mutableState.value = it }
    }

    private fun offline(detail: String): ConnectionState = ConnectionState(
        ConnectionMode.OFFLINE,
        false,
        detail,
    ).also { mutableState.value = it }
}
