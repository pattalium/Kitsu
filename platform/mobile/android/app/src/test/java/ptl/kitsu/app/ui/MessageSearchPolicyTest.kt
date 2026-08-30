package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageRoute

class MessageSearchPolicyTest {
    private val shadeKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    private val blockedKey = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE"

    @Test fun searchesRouteNameFullKeySenderAndMessageText() {
        val direct = thread(
            key = "direct:$shadeKey",
            route = MessageRoute.DIRECT,
            target = shadeKey,
            title = "Shade",
            subtitle = "Direct message",
            messages = listOf(message("Meet under the moon", shadeKey, "Shade")),
        )
        val channel = thread(
            key = "channel:0",
            route = MessageRoute.CHANNEL,
            target = "0",
            title = "Public",
            subtitle = "Channel messages · slot 0",
            messages = listOf(message("Weather is clear", null, "Copper Fox", channel = "0")),
        )
        val threads = listOf(direct, channel)

        assertEquals(listOf(direct), MessageSearchPolicy.filter(threads, "direct shade", emptySet()))
        assertEquals(listOf(direct), MessageSearchPolicy.filter(threads, shadeKey.take(18), emptySet()))
        assertEquals(listOf(direct), MessageSearchPolicy.filter(threads, "under moon", emptySet()))
        assertEquals(listOf(channel), MessageSearchPolicy.filter(threads, "channel public", emptySet()))
        assertEquals(listOf(channel), MessageSearchPolicy.filter(threads, "copper weather", emptySet()))
        assertEquals(threads, MessageSearchPolicy.filter(threads, "", emptySet()))
    }

    @Test fun blockedDirectThreadsAndBlockedChannelMessagesNeverBecomeSearchEvidence() {
        val direct = thread(
            key = "direct:$blockedKey",
            route = MessageRoute.DIRECT,
            target = blockedKey,
            title = "Blocked person",
            subtitle = "Direct message",
            messages = listOf(message("secret phrase", blockedKey, "Blocked person")),
        )
        val channel = thread(
            key = "channel:0",
            route = MessageRoute.CHANNEL,
            target = "0",
            title = "Public",
            subtitle = "Channel messages",
            messages = listOf(message("hidden channel phrase", blockedKey, "Blocked person", "0")),
        )

        assertTrue(MessageSearchPolicy.filter(listOf(direct, channel), "blocked", setOf(blockedKey)).isEmpty())
        assertTrue(MessageSearchPolicy.filter(listOf(direct, channel), "hidden", setOf(blockedKey)).isEmpty())
        assertEquals(listOf(channel), MessageSearchPolicy.filter(listOf(direct, channel), "public", setOf(blockedKey)))
    }

    private fun thread(
        key: String,
        route: MessageRoute,
        target: String,
        title: String,
        subtitle: String,
        messages: List<Message>,
    ) = MessageThread(
        key = key,
        route = route,
        target = target,
        title = title,
        subtitle = subtitle,
        messages = messages,
        unreadCount = 0,
        latestJournalPosition = 0,
    )

    private fun message(
        text: String,
        peerId: String?,
        sender: String,
        channel: String? = null,
    ) = Message(
        id = text,
        cursor = text,
        direction = "inbound",
        peerId = peerId,
        channel = channel,
        text = text,
        state = "received",
        occurredAt = 1_787_350_000,
        senderName = sender,
    )
}
