package ptl.kitsu.app.transport

/** Prevents fatal probing of encounter operations on firmware older than 0.17.0. */
internal object FirmwareEncounterApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun supportsV1(firmwareVersion: String?): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return false }
        return parts[0] > 0L || parts[1] >= 17L
    }
}
