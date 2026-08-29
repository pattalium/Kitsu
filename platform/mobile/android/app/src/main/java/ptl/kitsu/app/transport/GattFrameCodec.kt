package ptl.kitsu.app.transport

data class FrameDecodeResult(
    val frames: List<ByteArray> = emptyList(),
    val error: String? = null,
)

class GattFrameDecoder(
    private val maxPayloadBytes: Int = MAX_GATT_JSON_BYTES,
    private val timeoutMillis: Long = GATT_FRAME_TIMEOUT_MILLIS,
) {
    private val header = ByteArray(4)
    private var headerCount = 0
    private var body: ByteArray? = null
    private var bodyCount = 0
    private var startedAtMillis: Long? = null

    @Synchronized
    fun accept(fragment: ByteArray, nowMillis: Long): FrameDecodeResult {
        if (hasPartialFrame() && isExpired(nowMillis)) {
            reset()
            return FrameDecodeResult(error = "frame_timeout")
        }
        val completed = mutableListOf<ByteArray>()
        var offset = 0
        while (offset < fragment.size) {
            if (startedAtMillis == null) startedAtMillis = nowMillis
            if (headerCount < header.size) {
                val count = minOf(header.size - headerCount, fragment.size - offset)
                fragment.copyInto(header, headerCount, offset, offset + count)
                headerCount += count
                offset += count
                if (headerCount < header.size) continue

                val announced =
                    ((header[0].toInt() and 0xff) shl 24) or
                    ((header[1].toInt() and 0xff) shl 16) or
                    ((header[2].toInt() and 0xff) shl 8) or
                    (header[3].toInt() and 0xff)
                if (announced <= 0 || announced > maxPayloadBytes) {
                    reset()
                    return FrameDecodeResult(completed, "invalid_frame_length")
                }
                // Allocation happens only after the complete header passed the cap.
                body = ByteArray(announced)
                bodyCount = 0
            }

            val target = body ?: return FrameDecodeResult(completed, "decoder_state")
            val count = minOf(target.size - bodyCount, fragment.size - offset)
            fragment.copyInto(target, bodyCount, offset, offset + count)
            bodyCount += count
            offset += count
            if (bodyCount == target.size) {
                completed += target
                reset()
            }
        }
        return FrameDecodeResult(completed)
    }

    @Synchronized
    fun expire(nowMillis: Long): Boolean {
        if (!hasPartialFrame() || !isExpired(nowMillis)) return false
        reset()
        return true
    }

    @Synchronized
    fun hasPartialFrame(): Boolean = headerCount > 0 || body != null

    @Synchronized
    fun deadlineRemainingMillis(nowMillis: Long): Long? {
        if (!hasPartialFrame()) return null
        val started = startedAtMillis ?: return null
        return (timeoutMillis - (nowMillis - started)).coerceAtLeast(0L)
    }

    @Synchronized
    fun clear() = reset()

    private fun isExpired(nowMillis: Long): Boolean =
        nowMillis - (startedAtMillis ?: nowMillis) >= timeoutMillis

    private fun reset() {
        headerCount = 0
        body = null
        bodyCount = 0
        startedAtMillis = null
    }
}

fun encodeGattFrame(payload: ByteArray, maxPayloadBytes: Int = MAX_GATT_JSON_BYTES): ByteArray {
    require(payload.isNotEmpty()) { "empty_frame" }
    require(payload.size <= maxPayloadBytes) { "frame_too_large" }
    val length = payload.size
    return ByteArray(4 + length).also { frame ->
        frame[0] = (length ushr 24).toByte()
        frame[1] = (length ushr 16).toByte()
        frame[2] = (length ushr 8).toByte()
        frame[3] = length.toByte()
        payload.copyInto(frame, 4)
    }
}

const val MAX_GATT_JSON_BYTES = 16 * 1024
// Match the authenticated request bound. At the minimum BLE MTU a maximum
// response can span hundreds of notifications while the radio is active.
const val AUTHENTICATED_GATT_TIMEOUT_MILLIS = 30_000L
const val GATT_FRAME_TIMEOUT_MILLIS = AUTHENTICATED_GATT_TIMEOUT_MILLIS
