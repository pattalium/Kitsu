package ptl.kitsu.app.transport

/** Fun-state, expeditions, stories, and Party Hotspot arrived in firmware 0.19.0. */
internal object FirmwareFunApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun supportsV1(firmwareVersion: String?): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return false }
        return parts[0] > 0L || parts[1] >= 19L
    }
}
