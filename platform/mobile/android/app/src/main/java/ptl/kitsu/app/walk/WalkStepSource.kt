package ptl.kitsu.app.walk

import kotlinx.coroutines.flow.StateFlow

/** Runtime readiness of Android's built-in cumulative step sensor. */
enum class WalkStepAvailability {
    AVAILABLE,
    PERMISSION_REQUIRED,
    SENSOR_UNAVAILABLE,
    REGISTRATION_FAILED,
    CLOSED,
}

/** Live, route-bound absolute step total suitable for WalkSyncCommand.stepsTotal. */
data class WalkStepSnapshot(
    val availability: WalkStepAvailability,
    val observing: Boolean,
    /** True when a safe cumulative-counter baseline exists for the selected board/route. */
    val sensorBaselineReady: Boolean = false,
    /** Canonical stable address of the board currently selected for phone-step tracking. */
    val deviceAddress: String? = null,
    val routeId: Long? = null,
    val stepsTotal: Long = 0L,
    val requiredPermission: String? = null,
    val errorCode: String? = null,
) {
    val available: Boolean
        get() = availability == WalkStepAvailability.AVAILABLE

    val permissionRequired: Boolean
        get() = availability == WalkStepAvailability.PERMISSION_REQUIRED

    fun matches(deviceAddress: String, routeId: Long): Boolean =
        this.routeId == routeId &&
            this.deviceAddress?.trim()?.equals(deviceAddress.trim(), ignoreCase = true) == true
}

class WalkStepSourceException(
    val code: String,
    cause: Throwable? = null,
) : Exception(code, cause)

interface WalkStepSource : AutoCloseable {
    val snapshots: StateFlow<WalkStepSnapshot>

    /** Idempotently begins TYPE_STEP_COUNTER observation. */
    fun startObserving(): WalkStepSnapshot

    /** Stops sensor callbacks without forgetting the active route checkpoint. */
    fun stopObserving(): WalkStepSnapshot

    /** Selects a physical board and immediately stops crediting any previously selected board. */
    fun selectDevice(deviceAddress: String): WalkStepSnapshot

    /**
     * Binds phone deltas to one firmware route and its current absolute total.
     * Rebinding the same route is idempotent and never regresses a newer local total.
     */
    fun bindRoute(
        deviceAddress: String,
        routeId: Long,
        firmwareStepsTotal: Long,
    ): WalkStepSnapshot

    /** Clears only the exact board and route, so another board's same route ID is untouched. */
    fun clearRoute(deviceAddress: String, routeId: Long): WalkStepSnapshot

    /** Clears the selected board's retained route when firmware reports an idle route ID of zero. */
    fun clearDevice(deviceAddress: String): WalkStepSnapshot

    override fun close()
}

internal object WalkStepPermissionPolicy {
    const val ACTIVITY_RECOGNITION_PERMISSION = "android.permission.ACTIVITY_RECOGNITION"
    private const val ANDROID_10_API = 29

    fun permissionRequired(apiLevel: Int, permissionGranted: Boolean): Boolean =
        apiLevel >= ANDROID_10_API && !permissionGranted
}
