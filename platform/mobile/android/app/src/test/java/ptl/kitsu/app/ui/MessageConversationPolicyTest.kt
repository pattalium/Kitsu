package ptl.kitsu.app.ui

import java.time.LocalDate
import java.time.ZoneOffset
import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.ChannelRegionScope
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.Peer

class MessageConversationPolicyTest {
    private val peerId = "A".repeat(43)

    @Test fun groupsOnlyByAuthenticatedPeerOrChannelSlotNeverDisplayName() {
        val messages = listOf(
            message("1", session = "5", peer = peerId, sender = "Same name"),
            message("2", session = "5", channel = "0", sender = "Same name"),
        )

        val threads = MessageThreadPolicy.build(
            messages,
            peers = emptyList(),
            channels = listOf(MeshChannel(0, true, "Public")),
            blockedPeerIds = emptySet(),
            currentJournalSession = "5",
        )

        assertEquals(setOf("direct:$peerId", "channel:0"), threads.map { it.key }.toSet())
        assertEquals(1, threads.single { it.route == MessageRoute.DIRECT }.messages.size)
        assertEquals(1, threads.single { it.route == MessageRoute.CHANNEL }.messages.size)
    }

    @Test fun nearbyPeersWithoutHistoryStayInNewConversationInsteadOfFloodingRoot() {
        val threads = MessageThreadPolicy.build(
            messages = emptyList(),
            peers = listOf(Peer(peerId, "Copper Fox")),
            channels = listOf(MeshChannel(0, true, "Public")),
            blockedPeerIds = emptySet(),
        )

        assertEquals(listOf("channel:0"), threads.map { it.key })
    }

    @Test fun channelThreadPreservesAuthenticatedRoutingConfiguration() {
        val threads = MessageThreadPolicy.build(
            messages = emptyList(),
            peers = emptyList(),
            channels = listOf(MeshChannel(0, true, "Public", ChannelRegionScope.EU)),
            blockedPeerIds = emptySet(),
        )

        val thread = threads.single()
        assertTrue(thread.channelRoutingKnown)
        assertEquals(ChannelRegionScope.EU, thread.channelRegionScope)
        assertTrue(thread.subtitle.contains("Scoped #EU"))
    }

    @Test fun outboundSenderNameNeverBecomesRemoteDirectThreadIdentity() {
        val outbound = message(
            id = "1",
            session = "5",
            peer = peerId,
            sender = "Local Kitsu",
            direction = "outbound",
            state = "sent",
        )

        val thread = MessageThreadPolicy.build(
            listOf(outbound),
            peers = emptyList(),
            channels = emptyList(),
            blockedPeerIds = emptySet(),
            currentJournalSession = "5",
        ).single()

        assertEquals(MessageComposerPolicy.compactReference(peerId), thread.title)
    }

    @Test fun timelinePreservesFirmwareJournalOrderWhenTimestampsRegressOrAreSpoofed() {
        val first = message("1", "5", peer = peerId, occurredAt = 1_900_000_000L)
        val second = message("2", "5", peer = peerId, occurredAt = 1_800_000_000L)

        val bubbleIds = MessageThreadPolicy.timeline(
            listOf(first, second),
            zoneId = ZoneOffset.UTC,
            locale = Locale.US,
        ).filterIsInstance<ConversationTimelineItem.Bubble>().map { it.message.id }

        assertEquals(listOf("1", "2"), bubbleIds)
    }

    @Test fun stableUiKeysIncludeJournalSessionSoReusedRawIdsDoNotCollide() {
        assertEquals("5:1", message("1", "5", peer = peerId).uiStableJournalKey())
        assertEquals("6:1", message("1", "6", peer = peerId).uiStableJournalKey())
    }

    @Test fun unreadCountIncludesOnlyLiveCurrentSessionEntries() {
        val old = message("1", "4", peer = peerId, unread = true)
        val current = message("1", "5", peer = peerId, unread = true)

        val thread = MessageThreadPolicy.build(
            listOf(old, current),
            peers = emptyList(),
            channels = emptyList(),
            blockedPeerIds = emptySet(),
            currentJournalSession = "5",
        ).single()

        assertEquals(1, thread.unreadCount)
    }

    @Test fun timePolicyRejectsEpochFallbackAnd2100Boundary() {
        assertFalse(MessageTimePolicy.isAvailable(0))
        assertFalse(MessageTimePolicy.isAvailable(1_704_067_199L))
        assertTrue(MessageTimePolicy.isAvailable(1_704_067_200L))
        assertTrue(MessageTimePolicy.isAvailable(4_102_444_799L))
        assertFalse(MessageTimePolicy.isAvailable(4_102_444_800L))
    }

    @Test fun datePolicyUsesTodayYesterdayAndLocalizedAbsoluteFallback() {
        val today = LocalDate.of(2026, 8, 23)
        val todayEpoch = today.atStartOfDay().toEpochSecond(ZoneOffset.UTC)

        assertEquals(
            "Today",
            MessageTimePolicy.dateLabel(todayEpoch, ZoneOffset.UTC, Locale.US, today),
        )
        assertEquals(
            "Yesterday",
            MessageTimePolicy.dateLabel(todayEpoch - 86_400, ZoneOffset.UTC, Locale.US, today),
        )
        assertEquals(
            "Aug 21, 2026",
            MessageTimePolicy.dateLabel(todayEpoch - 2 * 86_400, ZoneOffset.UTC, Locale.US, today),
        )
        assertEquals(
            "Date unavailable",
            MessageTimePolicy.dateLabel(1, ZoneOffset.UTC, Locale.US, today),
        )
    }

    private fun message(
        id: String,
        session: String,
        peer: String? = null,
        channel: String? = null,
        sender: String = "Remote",
        direction: String = "inbound",
        state: String = "received",
        unread: Boolean? = false,
        occurredAt: Long = 1_787_350_000L + id.toLong(),
    ) = Message(
        id = id,
        cursor = id,
        direction = direction,
        peerId = peer,
        channel = channel,
        text = "message $id",
        state = state,
        occurredAt = occurredAt,
        revision = id,
        journalSession = session,
        senderName = sender,
        unreadOnKitsu = unread,
    )
}
