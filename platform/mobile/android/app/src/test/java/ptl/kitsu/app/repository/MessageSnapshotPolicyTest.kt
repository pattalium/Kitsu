package ptl.kitsu.app.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.cache.CachePolicy
import ptl.kitsu.app.model.Message

class MessageSnapshotPolicyTest {
    @Test fun v1RemainsASafeCompleteReplacementWithoutCrossBootAliasing() {
        val previous = listOf(message("1", "delivered", session = "8"))
        val legacy = listOf(message("1", "received", session = null))

        assertEquals(
            legacy,
            MessageSnapshotPolicy.merge(previous, snapshot(legacy, protocol = 1, session = null)),
        )
    }

    @Test fun firstTrustedV2SnapshotDropsUnnamespacedLegacyRows() {
        val previous = listOf(message("1", "received", session = null))
        val live = listOf(message("1", "received", session = "9"))

        val merged = MessageSnapshotPolicy.merge(previous, snapshot(live, session = "9"))

        assertEquals(listOf("9:1"), merged.map { it.stableJournalKey() })
    }

    @Test fun updatesSameSessionIdInPlaceWithoutReorderingHistory() {
        val previous = listOf(
            message("1", "sent", session = "9"),
            message("2", "received", session = "9"),
        )
        val current = listOf(
            message("1", "delivered", session = "9", revision = "4"),
            message("2", "received", session = "9", revision = "2"),
        )

        val merged = MessageSnapshotPolicy.merge(previous, snapshot(current, session = "9"))

        assertEquals(listOf("9:1", "9:2"), merged.map { it.stableJournalKey() })
        assertEquals("delivered", merged.first().state)
        assertEquals("4", merged.first().revision)
    }

    @Test fun v3SnapshotPreservesUpdatedLocalRepeatObservationOnTheSameJournalRow() {
        val before = message("1", "received", session = "9")
        val previousTarget = message("2", "sent", session = "9").copy(
            peerId = null,
            channel = "0",
            repeatCount = 0,
        )
        val after = message("3", "received", session = "9")
        val currentTarget = previousTarget.copy(revision = "4", repeatCount = 1)

        val merged = MessageSnapshotPolicy.merge(
            listOf(before, previousTarget, after),
            snapshot(
                listOf(before, currentTarget, after),
                protocol = 3,
                session = "9",
                revision = "4",
            ),
        )

        assertEquals(listOf("9:1", "9:2", "9:3"), merged.map { it.stableJournalKey() })
        assertEquals(3, merged.size)
        assertEquals("4", merged[1].revision)
        assertEquals(1, merged[1].repeatCount)
    }

    @Test fun reusedRawIdAcrossJournalSessionsNeverCollides() {
        val old = message("1", "received", session = "9")
        val rebooted = message("1", "received", session = "10")

        val merged = MessageSnapshotPolicy.merge(
            listOf(old),
            snapshot(listOf(rebooted), session = "10"),
        )

        assertEquals(listOf("9:1", "10:1"), merged.map { it.stableJournalKey() })
    }

    @Test fun evictedAndPriorSessionRowsCannotKeepUnclearablePhysicalUnreadBadges() {
        val priorSessionUnread = message("1", "received", session = "8", unread = true)
        val evictedSameSessionUnread = message("2", "received", session = "9", unread = true)
        val retainedLiveUnread = message("3", "received", session = "9", unread = true)

        val merged = MessageSnapshotPolicy.merge(
            listOf(priorSessionUnread, evictedSameSessionUnread, retainedLiveUnread),
            snapshot(listOf(retainedLiveUnread), session = "9"),
        )

        assertFalse(merged[0].unreadOnKitsu == true)
        assertFalse(merged[1].unreadOnKitsu == true)
        assertTrue(merged[2].unreadOnKitsu == true)
    }

    @Test fun emptyNewSessionRetainsArchivedConversationContentWithoutUnreadState() {
        val archived = message("7", "received", session = "9", unread = true)

        val merged = MessageSnapshotPolicy.merge(
            listOf(archived),
            snapshot(emptyList(), session = "10", revision = "0"),
        )

        assertEquals("hello 7", merged.single().text)
        assertEquals("9:7", merged.single().stableJournalKey())
        assertFalse(merged.single().unreadOnKitsu == true)
    }

    @Test fun retainedHistoryIsBoundedToEncryptedCacheMessageLimit() {
        val previous = (1..250).map { index ->
            message(index.toString(), "received", session = "8")
        }
        val live = (1..24).map { index ->
            message(index.toString(), "received", session = "9")
        }

        val merged = MessageSnapshotPolicy.merge(previous, snapshot(live, session = "9"))

        assertEquals(CachePolicy.MAX_MESSAGES, merged.size)
        assertEquals("8:19", merged.first().stableJournalKey())
        assertEquals("9:24", merged.last().stableJournalKey())
    }

    @Test fun legacyFullRingStillRejectsDuplicateIds() {
        val duplicated = message("1", "received", session = null)

        val failure = runCatching {
            MessageSnapshotPolicy.fullRing(listOf(duplicated, duplicated))
        }.exceptionOrNull()

        assertTrue(failure is IllegalArgumentException)
    }

    private fun snapshot(
        items: List<Message>,
        protocol: Int = 2,
        session: String?,
        revision: String = "20",
    ) = LoadedMessageSnapshot(
        items = items,
        protocolVersion = protocol,
        journalSession = session,
        journalRevision = if (protocol >= 2) revision else null,
    )

    private fun message(
        id: String,
        state: String,
        session: String?,
        revision: String = id,
        unread: Boolean? = if (state == "received" && session != null) false else null,
    ) = Message(
        id = id,
        cursor = id,
        direction = if (state == "received") "inbound" else "outbound",
        peerId = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        text = "hello $id",
        state = state,
        occurredAt = 1_787_350_000L + id.toLong(),
        revision = revision,
        journalSession = session,
        unreadOnKitsu = unread,
    )
}
