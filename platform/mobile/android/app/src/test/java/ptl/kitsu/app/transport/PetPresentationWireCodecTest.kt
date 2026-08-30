package ptl.kitsu.app.transport

import java.security.MessageDigest
import java.util.Base64
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.PetPresentationPlayback
import ptl.kitsu.app.model.PetPresentationRole
import ptl.kitsu.app.model.PetPresentationSurface

class PetPresentationWireCodecTest {
    @Test
    fun stateAcceptsExactBoundFrameContract() {
        val state = PetPresentationWireCodec.state(
            validState().encodeToByteArray(),
            expectedSessionId = 7L,
        )

        assertEquals(7L, state.sessionId)
        assertEquals(PetPresentationSurface.PET, state.surface)
        assertEquals(PetPresentationRole.IDLE, state.animation.resolvedRole)
        assertEquals(PetPresentationPlayback.LOOP, state.animation.playback)
        assertTrue(state.frame.available)
        assertEquals(512, state.frame.bytes)
    }

    @Test
    fun openStateMustEchoTheExactRequestedSession() {
        assertCode("malformed_presentation_state") {
            PetPresentationWireCodec.state(
                validState().encodeToByteArray(),
                expectedSessionId = 8L,
            )
        }
    }

    @Test
    fun absentPackIsTruthfulAndHasNoFrame() {
        val payload = """{"ok":true,"schema":1,"session_id":8,"captured_at_ms":0,"surface":"unknown","display_awake":false,"frame_visible":false,"pack":{"valid":false,"name":"None","id":0,"revision":0,"total_bytes":0,"payload_crc32":0,"header_crc32":0,"format":0,"width":0,"height":0,"frame_count":0,"appearance":0},"animation":{"active":false,"finite":false,"requested_role":"unknown","resolved_role":"unknown","playback":"unknown","token":0,"elapsed_ms":0},"frame":{"available":false,"encoding":"none","bytes":0,"sha256":""}}"""

        val state = PetPresentationWireCodec.state(payload.encodeToByteArray())

        assertFalse(state.pack.valid)
        assertFalse(state.frame.available)
    }

    @Test
    fun stateRejectsUnknownFieldsAndCrossFieldLies() {
        val unknown = validState().replace(
            "\"frame_visible\":true",
            "\"frame_visible\":true,\"extra\":1",
        )
        assertCode("malformed_presentation_state") {
            PetPresentationWireCodec.state(unknown.encodeToByteArray())
        }

        val invisibleFrame = validState().replace(
            "\"display_awake\":true",
            "\"display_awake\":false",
        )
        assertCode("invalid_presentation_visibility") {
            PetPresentationWireCodec.state(invisibleFrame.encodeToByteArray())
        }
    }

    @Test
    fun chunkBindsSessionRangeDigestAndCanonicalBytes() {
        val frame = ByteArray(512) { it.toByte() }
        val digest = sha256(frame)
        val data = frame.copyOfRange(0, 192)
        val encoded = Base64.getUrlEncoder().withoutPadding().encodeToString(data)
        val payload = """{"ok":true,"schema":1,"session_id":7,"offset":0,"bytes":192,"next_offset":192,"complete":false,"frame_sha256":"$digest","data_b64":"$encoded"}"""

        val chunk = PetPresentationWireCodec.chunk(
            payload.encodeToByteArray(),
            expectedSessionId = 7,
            expectedOffset = 0,
            expectedBytes = 192,
            expectedFrameBytes = 512,
            expectedFrameSha256 = digest,
        )

        assertArrayEquals(data, chunk.data)
        assertEquals(192, chunk.nextOffset)
        assertFalse(chunk.complete)
    }

    @Test
    fun chunkRejectsMismatchedAndNonCanonicalData() {
        val digest = "11".repeat(32)
        val payload = """{"ok":true,"schema":1,"session_id":7,"offset":0,"bytes":1,"next_offset":1,"complete":false,"frame_sha256":"$digest","data_b64":"AA=="}"""
        assertCode("malformed_presentation_chunk") {
            PetPresentationWireCodec.chunk(
                payload.encodeToByteArray(), 7, 0, 1, 512, digest,
            )
        }
    }

    @Test
    fun bodiesAndCloseUseBoundedUint32Session() {
        assertEquals("4294967295", PetPresentationWireCodec.openBody(4_294_967_295L)["session_id"].toString())
        assertCode("invalid_presentation_session") {
            PetPresentationWireCodec.openBody(4_294_967_296L)
        }
        assertTrue(
            PetPresentationWireCodec.closed(
                """{"ok":true,"schema":1,"closed":true}""".encodeToByteArray(),
            ),
        )
    }

    @Test
    fun fullFrameDigestVerificationIsExact() {
        val frame = ByteArray(512) { it.toByte() }
        val state = PetPresentationWireCodec.state(
            validState(sha256(frame)).encodeToByteArray(),
        )

        assertTrue(PetPresentationWireCodec.verifyFrame(state, frame))
        frame[0] = (frame[0].toInt() xor 1).toByte()
        assertFalse(PetPresentationWireCodec.verifyFrame(state, frame))
    }

    private fun validState(digest: String = "11".repeat(32)): String =
        """{"ok":true,"schema":1,"session_id":7,"captured_at_ms":123,"surface":"pet","display_awake":true,"frame_visible":true,"pack":{"valid":true,"name":"FOX GIRL","id":1234,"revision":3,"total_bytes":24976,"payload_crc32":12,"header_crc32":34,"format":1,"width":64,"height":64,"frame_count":48,"appearance":2},"animation":{"active":true,"finite":false,"requested_role":"idle","resolved_role":"idle","playback":"loop","token":9,"elapsed_ms":42},"frame":{"available":true,"encoding":"xbm_row_major_lsb_first","bytes":512,"sha256":"$digest"}}"""

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02X".format(it) }

    private fun assertCode(expected: String, block: () -> Unit) {
        val failure = assertThrows(TransportException::class.java) { block() }
        assertEquals(expected, failure.code)
    }
}
