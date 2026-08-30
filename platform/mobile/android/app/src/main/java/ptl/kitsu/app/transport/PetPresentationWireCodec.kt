package ptl.kitsu.app.transport

import java.security.MessageDigest
import java.util.Base64
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.put
import ptl.kitsu.app.model.PetPresentationChunk
import ptl.kitsu.app.model.PetPresentationPlayback
import ptl.kitsu.app.model.PetPresentationRole
import ptl.kitsu.app.model.PetPresentationState

internal object PetPresentationWireCodec {
    const val MAX_CHUNK_BYTES = 192
    const val MAX_FRAME_BYTES = 640
    private const val UINT32_MAX = 4_294_967_295L
    private val digestPattern = Regex("^[0-9A-F]{64}$")
    private val errorPattern = Regex("^[a-z][a-z0-9_]{0,63}$")
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = true
        isLenient = false
        coerceInputValues = false
    }

    fun openBody(sessionId: Long): JsonObject {
        requireSession(sessionId)
        return buildJsonObject { put("session_id", sessionId) }
    }

    fun readBody(
        sessionId: Long,
        offset: Int,
        bytes: Int,
        frameSha256: String,
    ): JsonObject {
        requireSession(sessionId)
        if (offset !in 0..MAX_FRAME_BYTES || bytes !in 1..MAX_CHUNK_BYTES ||
            offset + bytes > MAX_FRAME_BYTES || !digestPattern.matches(frameSha256)
        ) throw TransportException("invalid_presentation_read")
        return buildJsonObject {
            put("session_id", sessionId)
            put("offset", offset)
            put("bytes", bytes)
            put("frame_sha256", frameSha256)
        }
    }

    fun closeBody(sessionId: Long): JsonObject = openBody(sessionId)

    fun state(payload: ByteArray, expectedSessionId: Long? = null): PetPresentationState {
        val element = parseSuccess(payload, 2_048, "malformed_presentation_state")
        val root = element as? JsonObject ?: malformed("malformed_presentation_state")
        if (!root.exactKeys(
                "ok", "schema", "session_id", "captured_at_ms", "surface",
                "display_awake", "frame_visible", "pack", "animation", "frame",
            ) || !(root["pack"] as? JsonObject).hasKeys(
                "valid", "name", "id", "revision", "total_bytes", "payload_crc32",
                "header_crc32", "format", "width", "height", "frame_count", "appearance",
            ) || !(root["animation"] as? JsonObject).hasKeys(
                "active", "finite", "requested_role", "resolved_role", "playback",
                "token", "elapsed_ms",
            ) || !(root["frame"] as? JsonObject).hasKeys(
                "available", "encoding", "bytes", "sha256",
            )
        ) malformed("malformed_presentation_state")
        val value = decode<PetPresentationState>(element, "malformed_presentation_state")
        validateState(value)?.let { throw TransportException(it) }
        if (expectedSessionId != null && value.sessionId != expectedSessionId) {
            malformed("malformed_presentation_state")
        }
        return value
    }

    fun chunk(
        payload: ByteArray,
        expectedSessionId: Long,
        expectedOffset: Int,
        expectedBytes: Int,
        expectedFrameBytes: Int,
        expectedFrameSha256: String,
    ): PetPresentationChunk {
        val element = parseSuccess(payload, 1_024, "malformed_presentation_chunk")
        val root = element as? JsonObject ?: malformed("malformed_presentation_chunk")
        if (!root.exactKeys(
                "ok", "schema", "session_id", "offset", "bytes", "next_offset",
                "complete", "frame_sha256", "data_b64",
            )
        ) malformed("malformed_presentation_chunk")
        val wire = decode<ChunkWire>(element, "malformed_presentation_chunk")
        val decoded = runCatching { Base64.getUrlDecoder().decode(wire.dataB64) }
            .getOrElse { malformed("malformed_presentation_chunk") }
        val canonical = Base64.getUrlEncoder().withoutPadding().encodeToString(decoded)
        if (wire.schema != 1 || wire.sessionId != expectedSessionId ||
            wire.offset != expectedOffset || wire.bytes != expectedBytes ||
            wire.bytes != decoded.size || wire.nextOffset != wire.offset + wire.bytes ||
            wire.nextOffset > expectedFrameBytes ||
            wire.complete != (wire.nextOffset == expectedFrameBytes) ||
            wire.frameSha256 != expectedFrameSha256 || canonical != wire.dataB64
        ) malformed("malformed_presentation_chunk")
        return PetPresentationChunk(
            sessionId = wire.sessionId,
            offset = wire.offset,
            nextOffset = wire.nextOffset,
            complete = wire.complete,
            frameSha256 = wire.frameSha256,
            data = decoded,
        )
    }

    fun closed(payload: ByteArray): Boolean {
        val element = parseSuccess(payload, 128, "malformed_presentation_close")
        val root = element as? JsonObject ?: malformed("malformed_presentation_close")
        if (!root.exactKeys("ok", "schema", "closed")) {
            malformed("malformed_presentation_close")
        }
        val closed = (root["closed"] as? JsonPrimitive)?.booleanOrNull
        if ((root["schema"] as? JsonPrimitive)?.content != "1" || closed != true) {
            malformed("malformed_presentation_close")
        }
        return true
    }

    fun verifyFrame(state: PetPresentationState, frame: ByteArray): Boolean =
        state.frame.available && frame.size == state.frame.bytes &&
            sha256(frame) == state.frame.sha256

    private fun validateState(value: PetPresentationState): String? {
        if (!value.ok || value.schema != 1 || value.sessionId !in 1L..UINT32_MAX ||
            value.capturedAtMs !in 0L..UINT32_MAX
        ) return "invalid_presentation_header"
        val pack = value.pack
        val animation = value.animation
        val frame = value.frame
        if (!pack.valid) {
            if (pack.name != "None" || pack.id != 0L || pack.revision != 0L ||
                pack.totalBytes != 0L || pack.payloadCrc32 != 0L || pack.headerCrc32 != 0L ||
                pack.format != 0 || pack.width != 0 || pack.height != 0 ||
                pack.frameCount != 0 || pack.appearance != 0 || animation.active ||
                animation.finite || animation.requestedRole != PetPresentationRole.UNKNOWN ||
                animation.resolvedRole != PetPresentationRole.UNKNOWN ||
                animation.playback != PetPresentationPlayback.UNKNOWN || animation.token != 0L ||
                animation.elapsedMs != 0L || frame.available || value.frameVisible
            ) return "invalid_presentation_absent_pack"
            return validateMissingFrame(frame)
        }
        val expectedFrameBytes = when {
            pack.format == 1 && pack.width == 64 && pack.height == 64 -> 512
            pack.format == 2 && pack.width == 64 && pack.height == 80 -> 640
            else -> return "invalid_presentation_pack"
        }
        if (pack.name.isEmpty() || pack.name.length > 32 ||
            pack.name.any { it.code !in 0x20..0x7e } ||
            pack.id !in 1L..UINT32_MAX || pack.revision !in 1L..UINT32_MAX ||
            pack.totalBytes !in (65L..UINT32_MAX) || pack.payloadCrc32 !in 0L..UINT32_MAX ||
            pack.headerCrc32 !in 0L..UINT32_MAX || pack.frameCount !in 1..65_535 ||
            pack.totalBytes < 64L + pack.frameCount.toLong() * expectedFrameBytes ||
            pack.appearance !in 0..255
        ) return "invalid_presentation_pack"
        if (!animation.active) {
            if (animation.finite || animation.requestedRole != PetPresentationRole.UNKNOWN ||
                animation.resolvedRole != PetPresentationRole.UNKNOWN ||
                animation.playback != PetPresentationPlayback.UNKNOWN || animation.token != 0L ||
                animation.elapsedMs != 0L || frame.available || value.frameVisible
            ) return "invalid_presentation_animation"
            return validateMissingFrame(frame)
        }
        if (animation.requestedRole == PetPresentationRole.UNKNOWN ||
            animation.resolvedRole == PetPresentationRole.UNKNOWN ||
            animation.playback == PetPresentationPlayback.UNKNOWN ||
            animation.token !in 1L..UINT32_MAX || animation.elapsedMs !in 0L..UINT32_MAX
        ) return "invalid_presentation_animation"
        if (frame.available) {
            if (frame.encoding != "xbm_row_major_lsb_first" ||
                frame.bytes != expectedFrameBytes || !digestPattern.matches(frame.sha256)
            ) return "invalid_presentation_frame"
        } else {
            validateMissingFrame(frame)?.let { return it }
        }
        if (value.frameVisible && (!value.displayAwake || !frame.available)) {
            return "invalid_presentation_visibility"
        }
        return null
    }

    private fun validateMissingFrame(frame: ptl.kitsu.app.model.PetPresentationFrame): String? =
        if (!frame.available && frame.encoding == "none" && frame.bytes == 0 && frame.sha256.isEmpty()) {
            null
        } else {
            "invalid_presentation_frame"
        }

    private fun parseSuccess(payload: ByteArray, maxBytes: Int, malformedCode: String): JsonElement {
        if (payload.isEmpty() || payload.size > maxBytes) malformed(malformedCode)
        val text = payload.toString(Charsets.UTF_8)
        val element = runCatching { json.parseToJsonElement(text) }
            .getOrElse { malformed(malformedCode) }
        val root = element as? JsonObject ?: malformed(malformedCode)
        if ((root["ok"] as? JsonPrimitive)?.booleanOrNull != true) {
            if (root.keys != ERROR_KEYS) malformed(malformedCode)
            val error = (root["error"] as? JsonPrimitive)?.content
            if (error == null || !errorPattern.matches(error)) malformed(malformedCode)
            throw TransportException(error)
        }
        return element
    }

    private inline fun <reified T> decode(element: JsonElement, code: String): T =
        runCatching { json.decodeFromJsonElement<T>(element) }
            .getOrElse { malformed(code) }

    private fun requireSession(sessionId: Long) {
        if (sessionId !in 1L..UINT32_MAX) throw TransportException("invalid_presentation_session")
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02X".format(it) }

    private fun JsonObject.exactKeys(vararg expected: String): Boolean = keys == expected.toSet()
    private fun JsonObject?.hasKeys(vararg expected: String): Boolean =
        this?.exactKeys(*expected) == true

    private fun malformed(code: String): Nothing = throw TransportException(code)

    @Serializable
    private data class ChunkWire(
        val ok: Boolean,
        val schema: Int,
        @SerialName("session_id") val sessionId: Long,
        val offset: Int,
        val bytes: Int,
        @SerialName("next_offset") val nextOffset: Int,
        val complete: Boolean,
        @SerialName("frame_sha256") val frameSha256: String,
        @SerialName("data_b64") val dataB64: String,
    )

    private val ERROR_KEYS = setOf("ok", "error")
}
