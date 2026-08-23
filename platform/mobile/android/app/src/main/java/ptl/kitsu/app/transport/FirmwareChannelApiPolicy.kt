package ptl.kitsu.app.transport

/** Selects the exact authenticated channel catalog operation from the trusted firmware version. */
internal object FirmwareChannelApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun protocolVersion(firmwareVersion: String?): Int {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return 1
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return 1 }
        return if (
            parts[0] > 0L || parts[1] > 16L ||
            (parts[1] == 16L && parts[2] >= 4L)
        ) 2 else 1
    }

    fun operation(protocolVersion: Int): String = when (protocolVersion) {
        1 -> "channels.get"
        2 -> "channels.get.v2"
        else -> throw IllegalArgumentException("unsupported_channel_protocol")
    }
}
