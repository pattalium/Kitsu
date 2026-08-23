package ptl.kitsu.app.repository

import ptl.kitsu.app.model.MessageMarkReadReceipt

/** Binds an authenticated receipt to the exact immutable session/request batch. */
internal object MessageReadReceiptPolicy {
    fun matches(
        receipt: MessageMarkReadReceipt,
        expectedSession: String,
        expectedCount: Int,
    ): Boolean = receipt.accepted &&
        receipt.journalSession == expectedSession &&
        receipt.markedCount + receipt.unchangedCount == expectedCount
}
