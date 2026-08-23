package ptl.kitsu.app.transport

/** Selects a non-probeable authenticated operation from the trusted state response. */
internal object FirmwareMessageApiPolicy {
    private val VERSION = Regex("^(\\d+)\\.(\\d+)\\.(\\d+)(?:[-+].*)?$")

    fun protocolVersion(firmwareVersion: String?): Int {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return 1
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return 1 }
        return when {
            parts[0] > 0L || parts[1] > 16L || (parts[1] == 16L && parts[2] >= 1L) -> 4
            parts[1] == 16L -> 3
            parts[1] > 14L || (parts[1] == 14L && parts[2] >= 0L) -> 2
            else -> 1
        }
    }

    fun operation(protocolVersion: Int): String = when (protocolVersion) {
        1 -> "messages.get"
        2 -> "messages.get.v2"
        3 -> "messages.get.v3"
        4 -> "messages.get.v4"
        else -> throw IllegalArgumentException("unsupported_message_protocol")
    }

    fun supportsMarkRead(firmwareVersion: String?): Boolean =
        semanticVersionAtLeast(firmwareVersion, requiredMinor = 15)

    fun requireMarkReadRequest(journalSession: String, messageIds: List<String>) {
        require(canonicalUint32(journalSession)) { "message_session_invalid" }
        require(messageIds.size in 1..24) { "message_read_batch_invalid" }
        require(messageIds.distinct().size == messageIds.size) { "message_read_batch_duplicate" }
        require(messageIds.all(::canonicalUint32)) { "message_read_id_invalid" }
    }

    private fun semanticVersionAtLeast(firmwareVersion: String?, requiredMinor: Long): Boolean {
        val match = firmwareVersion?.let(VERSION::matchEntire) ?: return false
        val parts = match.groupValues.drop(1).take(3).map { it.toLongOrNull() ?: return false }
        return parts[0] > 0L || parts[1] >= requiredMinor
    }

    private fun canonicalUint32(value: String): Boolean = value.toLongOrNull()?.let {
        it in 1L..0xffff_ffffL && value == it.toString()
    } == true
}
