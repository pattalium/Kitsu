package ptl.kitsu.app.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message

class OwnerRefreshCommitPolicyTest {
    @Test fun delayedBroadRefreshPreservesAnInterveningMessageCommitAndItsFreshness() {
        val delivered = message("delivered")
        val current = OwnerState(
            messages = listOf(delivered),
            messagesErrorCode = null,
            errorCode = "history_timeout",
        )
        val broadReadStartedEarlier = OwnerNonMessageSnapshot(
            connection = ConnectionState(),
            status = KitsuStatus(deviceId = "KT0001", companionName = "Kitsu", updatedAt = 2),
            history = listOf(HistoryEntry("2", "2", "mesh", "heard", 2)),
            peers = emptyList(),
            channels = emptyList(),
        )

        val committed = OwnerRefreshCommitPolicy.apply(current, broadReadStartedEarlier)

        assertEquals(listOf(delivered), committed.messages)
        assertNull(committed.messagesErrorCode)
        assertEquals("KT0001", committed.status?.deviceId)
        assertNull(committed.errorCode)
    }

    @Test fun unrelatedEndpointFailureCannotDiscardAValidMessageSnapshot() {
        val received = message("received")
        val current = OwnerState(messages = listOf(received), messagesErrorCode = null, loading = true)

        val failed = OwnerRefreshCommitPolicy.applyFailure(
            current = current,
            connection = ConnectionState(),
            errorCode = "malformed_history",
        )

        assertEquals(listOf(received), failed.messages)
        assertNull(failed.messagesErrorCode)
        assertEquals("malformed_history", failed.errorCode)
    }

    private fun message(state: String) = Message(
        id = "4",
        cursor = "4",
        direction = "outbound",
        text = "hello",
        state = state,
        occurredAt = 1,
    )
}
