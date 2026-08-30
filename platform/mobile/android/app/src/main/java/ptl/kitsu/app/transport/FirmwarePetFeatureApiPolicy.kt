package ptl.kitsu.app.transport

/**
 * Profile/check-in, focus, and real-walk v1 operations ship together in 0.20.4.
 * Older firmware must stay on its known API surface instead of being probed.
 */
internal object FirmwarePetFeatureApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")
    private val MINIMUM = listOf(0L, 20L, 4L)

    fun supportsV1(firmwareVersion: String?): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val current = match.groupValues.drop(1).take(3).map { component ->
            component.toLongOrNull() ?: return false
        }
        for (index in MINIMUM.indices) {
            if (current[index] != MINIMUM[index]) {
                return current[index] > MINIMUM[index]
            }
        }
        return true
    }
}
