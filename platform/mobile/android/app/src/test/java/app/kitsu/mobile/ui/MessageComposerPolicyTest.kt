package app.kitsu.mobile.ui

import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.Peer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MessageComposerPolicyTest {
    private val validKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun byteLimitCountsUtf8InsteadOfCharacters() {
        val ascii = "a".repeat(128)
        val emoji = "🦊".repeat(32)
        assertEquals(128, MessageComposerPolicy.utf8Bytes(ascii))
        assertEquals(128, MessageComposerPolicy.utf8Bytes(emoji))
        assertEquals(ascii, MessageComposerPolicy.acceptText("old", ascii))
        assertEquals(emoji, MessageComposerPolicy.acceptText("old", emoji))
        assertEquals("old", MessageComposerPolicy.acceptText("old", "$emoji🦊"))
    }

    @Test fun directRecipientMustBeFullUnpaddedBase64UrlPublicKey() {
        assertNull(MessageComposerPolicy.validationError(MessageRoute.DIRECT, validKey, "hello"))
        assertTrue(
            MessageComposerPolicy.validationError(MessageRoute.DIRECT, "peer-1", "hello")!!
                .contains("43-character"),
        )
        assertTrue(
            MessageComposerPolicy.validationError(MessageRoute.DIRECT, "$validKey=", "hello")!!
                .contains("43-character"),
        )
        assertTrue(
            MessageComposerPolicy.validationError(
                MessageRoute.DIRECT,
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB",
                "hello",
            ) != null,
        )
    }

    @Test fun channelRecipientIsCanonicalSlotAndOnlyKnownConfiguredMetadataIsOffered() {
        assertNull(MessageComposerPolicy.validationError(MessageRoute.CHANNEL, "0", "hello"))
        assertNull(MessageComposerPolicy.validationError(MessageRoute.CHANNEL, "3", "hello"))
        assertTrue(MessageComposerPolicy.validationError(MessageRoute.CHANNEL, "04", "hello") != null)
        val recipients = MessageComposerPolicy.channelRecipients(
            listOf(
                MeshChannel(0, true, "Public"),
                MeshChannel(1, false, "Private"),
                MeshChannel(2, true, "Ops"),
                MeshChannel(2, true, "Duplicate"),
                MeshChannel(3, null, null),
            ),
        )
        assertEquals(listOf("0", "2"), recipients.map { it.reference })
        assertEquals("Public · slot 0", recipients.first().label)
    }

    @Test fun contactSuggestionsExcludeDisplayNamesAndMalformedIds() {
        val recipients = MessageComposerPolicy.contactRecipients(
            listOf(
                Peer(validKey, "Alice"),
                Peer("Alice", "Display name only"),
                Peer(validKey, "Duplicate"),
                Peer("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", "Repeater", role = "repeater"),
            ),
        )
        assertEquals(1, recipients.size)
        assertEquals(validKey, recipients.single().reference)
        assertEquals("Alice", recipients.single().label)
    }

    @Test fun deliveryAndUnreadUseAuthoritativeWireStateOnly() {
        val messages = listOf(
            message("m1", state = "unread"),
            message("m2", state = "received"),
            message("m3", direction = "outbound", state = "unread"),
            message("m4", state = "new"),
        )
        assertEquals(2, MessageComposerPolicy.unreadCount(messages))
        assertEquals(MessageDelivery.DELIVERED, MessageComposerPolicy.delivery("acked"))
        assertEquals(MessageDelivery.FAILED, MessageComposerPolicy.delivery("expired"))
        assertEquals(MessageDelivery.UNKNOWN, MessageComposerPolicy.delivery("custom_state"))
    }

    @Test fun blankAndOversizedMessagesAreRejectedWithoutTrimmingContent() {
        assertEquals(
            "Write a message",
            MessageComposerPolicy.validationError(MessageRoute.DIRECT, validKey, "   "),
        )
        assertTrue(
            MessageComposerPolicy.validationError(
                MessageRoute.DIRECT,
                validKey,
                "x".repeat(129),
            )!!.contains("128"),
        )
    }

    private fun message(
        id: String,
        direction: String = "inbound",
        channel: String? = null,
        state: String = "received",
    ) = Message(
        id = id,
        cursor = id,
        direction = direction,
        channel = channel,
        text = "hello",
        state = state,
        occurredAt = 1,
    )
}
