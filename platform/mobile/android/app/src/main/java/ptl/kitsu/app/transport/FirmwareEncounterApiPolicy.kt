package ptl.kitsu.app.transport

/** Prevents fatal probing of encounter operations on firmware older than 0.17.0. */
internal object FirmwareEncounterApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun supportsV1(firmwareVersion: String?): Boolean {
        return semanticVersionAtLeast(firmwareVersion, 0L, 17L, 0L)
    }

    /** The bounded public catalog operation was added one release after encounter v1. */
    fun supportsCatalogV1(firmwareVersion: String?): Boolean {
        return semanticVersionAtLeast(firmwareVersion, 0L, 18L, 0L)
    }

    /** Discovery observations became an authenticated, read-only API in 0.20.2. */
    fun supportsDiscoveryV1(firmwareVersion: String?): Boolean {
        return semanticVersionAtLeast(firmwareVersion, 0L, 20L, 2L)
    }

    private fun semanticVersionAtLeast(
        firmwareVersion: String?,
        requiredMajor: Long,
        requiredMinor: Long,
        requiredPatch: Long,
    ): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return false }
        return compareValuesBy(
            parts,
            listOf(requiredMajor, requiredMinor, requiredPatch),
            { it[0] },
            { it[1] },
            { it[2] },
        ) >= 0
    }
}
