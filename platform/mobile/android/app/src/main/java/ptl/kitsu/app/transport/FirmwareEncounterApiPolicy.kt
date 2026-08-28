package ptl.kitsu.app.transport

/** Prevents fatal probing of encounter operations on firmware older than 0.17.0. */
internal object FirmwareEncounterApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun supportsV1(firmwareVersion: String?): Boolean {
        return semanticVersionAtLeast(firmwareVersion, requiredMinor = 17L)
    }

    /** The bounded public catalog operation was added one release after encounter v1. */
    fun supportsCatalogV1(firmwareVersion: String?): Boolean {
        return semanticVersionAtLeast(firmwareVersion, requiredMinor = 18L)
    }

    private fun semanticVersionAtLeast(firmwareVersion: String?, requiredMinor: Long): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return false }
        return parts[0] > 0L || parts[1] >= requiredMinor
    }
}
