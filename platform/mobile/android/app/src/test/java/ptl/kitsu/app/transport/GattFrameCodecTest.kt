package ptl.kitsu.app.transport

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GattFrameCodecTest {
    @Test fun fragmentedHeaderAndBodyReassembleWithoutDelimiterAssumptions() {
        val payload = "{\"text\":\"line one\\nline two\"}".toByteArray()
        val frame = encodeGattFrame(payload)
        val decoder = GattFrameDecoder()
        assertTrue(decoder.accept(frame.copyOfRange(0, 2), 0).frames.isEmpty())
        assertTrue(decoder.accept(frame.copyOfRange(2, 7), 1).frames.isEmpty())
        val complete = decoder.accept(frame.copyOfRange(7, frame.size), 2)
        assertEquals(null, complete.error)
        assertArrayEquals(payload, complete.frames.single())
        assertFalse(decoder.hasPartialFrame())
    }

    @Test fun oversizeIsRejectedImmediatelyAfterHeader() {
        val decoder = GattFrameDecoder(maxPayloadBytes = 16)
        val result = decoder.accept(byteArrayOf(0, 0, 0, 17), 0)
        assertEquals("invalid_frame_length", result.error)
        assertFalse(decoder.hasPartialFrame())
    }

    @Test fun incompleteFrameExpires() {
        val decoder = GattFrameDecoder(timeoutMillis = 10)
        decoder.accept(byteArrayOf(0, 0, 0, 2, 1), 100)
        assertFalse(decoder.expire(109))
        assertTrue(decoder.hasPartialFrame())
        assertTrue(decoder.expire(110))
        assertFalse(decoder.hasPartialFrame())
    }

    @Test fun productionDeadlineMatchesFirmwareBoundary() {
        val decoder = GattFrameDecoder()
        decoder.accept(byteArrayOf(0, 0, 0, 2, 1), 0)
        assertFalse(decoder.expire(9_999))
        assertTrue(decoder.expire(10_000))
    }
}
