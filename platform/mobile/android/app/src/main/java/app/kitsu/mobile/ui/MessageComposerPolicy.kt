package app.kitsu.mobile.ui

import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshPeerKeyPolicy
import app.kitsu.mobile.model.Peer

const val MAX_MESSAGE_UTF8_BYTES = 128

data class MessageRecipient(
    val reference: String,
    val label: String,
)

enum class MessageDelivery {
    QUEUED,
    SENT,
    DELIVERED,
    FAILED,
    UNREAD,
    RECEIVED,
    UNKNOWN,
}

object MessageComposerPolicy {
    private val channelSlot = Regex("^[0-3]$")

    fun utf8Bytes(text: String): Int = text.toByteArray(Charsets.UTF_8).size

    fun acceptText(current: String, candidate: String): String =
        if (utf8Bytes(candidate) <= MAX_MESSAGE_UTF8_BYTES) candidate else current

    fun validationError(route: MessageRoute, target: String, text: String): String? = when {
        target.trim().isEmpty() -> if (route == MessageRoute.DIRECT) {
            "Choose a contact or enter a public-key reference"
        } else {
            "Choose or enter a channel slot/reference"
        }
        route == MessageRoute.DIRECT && !MeshPeerKeyPolicy.isCanonicalBase64Url(target.trim()) ->
            "Contact must be the complete 43-character public-key reference"
        route == MessageRoute.CHANNEL && !channelSlot.matches(target.trim()) ->
            "Channel must be a canonical slot from 0 to 3"
        text.isBlank() -> "Write a message"
        utf8Bytes(text) > MAX_MESSAGE_UTF8_BYTES -> "Message exceeds $MAX_MESSAGE_UTF8_BYTES UTF-8 bytes"
        else -> null
    }

    fun contactRecipients(peers: List<Peer>): List<MessageRecipient> = peers
        .asSequence()
        .filter {
            it.role.equals("client", ignoreCase = true) &&
                MeshPeerKeyPolicy.isCanonicalBase64Url(it.id)
        }
        .distinctBy { it.id }
        .map { MessageRecipient(it.id, it.name.ifBlank { compactReference(it.id) }) }
        .toList()

    fun channelRecipients(channels: List<MeshChannel>): List<MessageRecipient> = channels
        .asSequence()
        .filter { it.configured == true && it.slot in 0..3 }
        .sortedBy { it.slot }
        .map {
            MessageRecipient(
                it.slot.toString(),
                it.name?.takeIf(String::isNotBlank)?.let { name -> "$name · slot ${it.slot}" }
                    ?: "Slot ${it.slot}",
            )
        }
        .distinctBy { it.reference }
        .toList()

    fun delivery(state: String): MessageDelivery = when (state.trim().lowercase()) {
        "queued", "pending", "accepted" -> MessageDelivery.QUEUED
        "sent", "transmitted" -> MessageDelivery.SENT
        "delivered", "acknowledged", "acked" -> MessageDelivery.DELIVERED
        "failed", "rejected", "expired", "undeliverable" -> MessageDelivery.FAILED
        "unread", "new" -> MessageDelivery.UNREAD
        "received", "read" -> MessageDelivery.RECEIVED
        else -> MessageDelivery.UNKNOWN
    }

    fun unreadCount(messages: List<Message>): Int = messages.count {
        it.direction.equals("inbound", ignoreCase = true) && delivery(it.state) == MessageDelivery.UNREAD
    }

    fun compactReference(value: String): String {
        val trimmed = value.trim()
        return if (trimmed.length <= 14) trimmed else "${trimmed.take(7)}…${trimmed.takeLast(5)}"
    }
}
