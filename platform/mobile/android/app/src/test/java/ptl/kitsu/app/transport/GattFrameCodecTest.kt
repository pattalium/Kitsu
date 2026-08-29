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
        assertEquals(30_000L, decoder.deadlineRemainingMillis(0))
        assertFalse(decoder.expire(29_999))
        assertEquals(1L, decoder.deadlineRemainingMillis(29_999))
        assertTrue(decoder.expire(30_000))
    }

    @Test fun maximumMtu23ResponseMaySpanMoreThanTenSecondsWithinTheRealBound() {
        val payload = ByteArray(MAX_GATT_JSON_BYTES) { (it and 0xff).toByte() }
        val frame = encodeGattFrame(payload)
        val decoder = GattFrameDecoder()
        var offset = 0
        var now = 0L
        var complete = FrameDecodeResult()
        while (offset < frame.size) {
            val end = minOf(offset + 20, frame.size)
            complete = decoder.accept(frame.copyOfRange(offset, end), now)
            assertEquals(null, complete.error)
            offset = end
            now += 15L
        }

        assertTrue(now > 10_000L)
        assertTrue(now < GATT_FRAME_TIMEOUT_MILLIS)
        assertArrayEquals(payload, complete.frames.single())
        assertFalse(decoder.hasPartialFrame())
    }

    @Test fun disconnectClearPreventsOldFragmentsFromPoisoningAReplacementLink() {
        val decoder = GattFrameDecoder(timeoutMillis = 30_000)
        val oldFrame = encodeGattFrame("old-link".toByteArray())
        decoder.accept(oldFrame.copyOfRange(0, 6), nowMillis = 1)
        assertTrue(decoder.hasPartialFrame())

        decoder.clear()
        assertFalse(decoder.hasPartialFrame())
        assertFalse(decoder.expire(nowMillis = 30_001))

        val replacementPayload = "replacement-link".toByteArray()
        val replacement = decoder.accept(encodeGattFrame(replacementPayload), nowMillis = 30_002)
        assertEquals(null, replacement.error)
        assertArrayEquals(replacementPayload, replacement.frames.single())
    }
}
