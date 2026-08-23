package ptl.kitsu.app.repository

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.MessageMarkReadReceipt

class MessageReadReceiptPolicyTest {
    @Test fun receiptMustMatchExactSessionAndSubmittedCount() {
        val exact = receipt(session = "7", marked = 2, unchanged = 1)

        assertTrue(MessageReadReceiptPolicy.matches(exact, "7", 3))
        assertFalse(MessageReadReceiptPolicy.matches(exact, "8", 3))
        assertFalse(MessageReadReceiptPolicy.matches(exact, "7", 2))
        assertFalse(MessageReadReceiptPolicy.matches(exact.copy(accepted = false), "7", 3))
    }

    private fun receipt(session: String, marked: Int, unchanged: Int) = MessageMarkReadReceipt(
        schema = "kitsu.messages-mark-read.v1",
        accepted = true,
        markedCount = marked,
        unchangedCount = unchanged,
        journalSession = session,
        journalRevision = "22",
    )
}
