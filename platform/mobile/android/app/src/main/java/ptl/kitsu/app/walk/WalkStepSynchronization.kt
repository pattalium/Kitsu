package ptl.kitsu.app.walk

import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkPhase

internal data class WalkStepSyncRequest(
    val deviceAddress: String,
    val routeId: Long,
    val stepsTotal: Long,
)

/** Pure eligibility and exact-device matching rules shared by automatic and terminal sync. */
internal object WalkStepSyncPolicy {
    fun automaticRequest(
        snapshot: WalkStepSnapshot,
        deviceAddress: String?,
        walk: WalkAdventureState?,
        connected: Boolean,
        firmwareControlsLocked: Boolean,
    ): WalkStepSyncRequest? {
        if (!connected || firmwareControlsLocked || deviceAddress == null || walk == null) return null
        if (walk.phase != WalkPhase.ACTIVE && walk.phase != WalkPhase.AWAITING_RESCUE) return null
        if (snapshot.availability != WalkStepAvailability.AVAILABLE || !snapshot.observing) return null
        if (!snapshot.matches(deviceAddress, walk.routeId) || snapshot.stepsTotal <= walk.steps) return null
        return WalkStepSyncRequest(
            deviceAddress = WalkStepDeviceIdentity.normalize(deviceAddress),
            routeId = walk.routeId,
            stepsTotal = snapshot.stepsTotal,
        )
    }

    fun terminalPhoneTotal(
        snapshot: WalkStepSnapshot,
        deviceAddress: String,
        walk: WalkAdventureState,
    ): Long? = snapshot.stepsTotal.takeIf {
        snapshot.matches(deviceAddress, walk.routeId) && it > walk.steps
    }
}

/** Edge detector used so OTA completion retries the unchanged latest sensor snapshot once. */
internal class WalkFirmwareUnlockRetry(initiallyLocked: Boolean) {
    private var wasLocked = initiallyLocked

    fun observe(locked: Boolean): Boolean {
        val retry = wasLocked && !locked
        wasLocked = locked
        return retry
    }
}

/** Serializes phone-step BLE writes with terminal mutations and the entire firmware install. */
internal class WalkStepOperationGate {
    private val mutex = Mutex()

    suspend fun <T> withWalkOperation(block: suspend () -> T): T = mutex.withLock { block() }

    suspend fun <T> withFirmwareOperation(block: suspend () -> T): T = mutex.withLock { block() }

    suspend fun <T> withTerminalOperation(
        deviceAddress: String,
        walk: WalkAdventureState,
        snapshot: () -> WalkStepSnapshot,
        validateBinding: () -> Unit,
        sync: suspend (stepsTotal: Long) -> WalkAdventureState,
        terminal: suspend () -> T,
    ): T = mutex.withLock {
        validateBinding()
        WalkStepSyncPolicy.terminalPhoneTotal(snapshot(), deviceAddress, walk)?.let { phoneTotal ->
            val synced = sync(phoneTotal)
            check(synced.routeId == walk.routeId) { "walk_route_changed_during_terminal_sync" }
        }
        validateBinding()
        terminal()
    }
}
