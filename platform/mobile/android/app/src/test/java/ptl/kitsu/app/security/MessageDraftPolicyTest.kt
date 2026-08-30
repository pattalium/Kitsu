package ptl.kitsu.app.security

import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MessageDraftPolicyTest {
    private val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun draftsAreBoundToNormalizedDeviceAndCanonicalThread() {
        val records = MessageDraftPolicy.upsert(
            records = emptyList(),
            deviceAddress = "aa:bb:cc:dd:ee:ff",
            threadKey = "direct:$peer",
            text = "hello",
            updatedAtMillis = 10,
        )

        assertEquals(
            mapOf("direct:$peer" to "hello"),
            MessageDraftPolicy.forDevice(records, "AA:BB:CC:DD:EE:FF"),
        )
        assertTrue(MessageDraftPolicy.forDevice(records, "11:22:33:44:55:66").isEmpty())
        assertTrue(MessageDraftPolicy.validThreadKey("channel:0"))
        assertTrue(MessageDraftPolicy.validThreadKey("channel:3"))
        assertFalse(MessageDraftPolicy.validThreadKey("channel:4"))
        assertFalse(MessageDraftPolicy.validThreadKey("direct:Shade"))
    }

    @Test fun clearingRemovesOnlyTheExactDeviceAndThreadBinding() {
        val first = MessageDraftPolicy.upsert(
            emptyList(), "AA", "direct:$peer", "direct draft", 1,
        )
        val second = MessageDraftPolicy.upsert(first, "AA", "channel:0", "channel draft", 2)
        val otherDevice = MessageDraftPolicy.upsert(second, "BB", "direct:$peer", "other", 3)
        val cleared = MessageDraftPolicy.upsert(otherDevice, "AA", "direct:$peer", "", 4)

        assertEquals(mapOf("channel:0" to "channel draft"), MessageDraftPolicy.forDevice(cleared, "AA"))
        assertEquals(mapOf("direct:$peer" to "other"), MessageDraftPolicy.forDevice(cleared, "BB"))
    }

    @Test fun utf8LimitAndPerDeviceBoundAreEnforced() {
        assertTrue(MessageDraftPolicy.validText("🦊".repeat(32)))
        assertFalse(MessageDraftPolicy.validText("🦊".repeat(33)))

        var records = emptyList<MessageDraftRecord>()
        repeat(40) { index ->
            val key = Base64.getUrlEncoder().withoutPadding().encodeToString(
                ByteArray(32) { byte -> (index * 31 + byte).toByte() },
            )
            records = MessageDraftPolicy.upsert(
                records,
                deviceAddress = "device-a",
                threadKey = "direct:$key",
                text = "draft-$index",
                updatedAtMillis = index.toLong(),
            )
        }
        assertEquals(MessageDraftPolicy.MAX_RECORDS_PER_DEVICE, records.size)

        val deliberatelyDuplicated = (0 until 40).map { index ->
            MessageDraftRecord(
                deviceAddress = "device-$index",
                threadKey = "channel:0",
                text = "draft-$index",
                updatedAtMillis = index.toLong(),
            )
        }
        assertEquals(40, MessageDraftPolicy.bounded(deliberatelyDuplicated).size)
    }

    @Test fun duplicateBindingsKeepTheNewestValidRecord() {
        val records = listOf(
            MessageDraftRecord("AA", "direct:$peer", "new", 20),
            MessageDraftRecord("aa", "direct:$peer", "old", 10),
        )
        assertEquals(
            mapOf("direct:$peer" to "new"),
            MessageDraftPolicy.forDevice(MessageDraftPolicy.bounded(records), "AA"),
        )
    }
}
