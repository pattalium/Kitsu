package ptl.kitsu.app.ui

import java.text.Normalizer
import java.util.Locale
import ptl.kitsu.app.model.MessageRoute

/** Local search over the already bounded, already authorized conversation snapshot. */
internal object MessageSearchPolicy {
    fun filter(
        threads: List<MessageThread>,
        query: String,
        blockedPeerIds: Set<String>,
    ): List<MessageThread> {
        val terms = normalize(query).split(WHITESPACE).filter(String::isNotEmpty)
        return threads.filter { thread ->
            if (thread.route == MessageRoute.DIRECT && thread.target in blockedPeerIds) {
                return@filter false
            }
            val visibleMessages = thread.messages.filterNot { it.peerId in blockedPeerIds }
            if (terms.isEmpty()) return@filter true
            val searchable = buildString {
                append(thread.title)
                append('\n')
                append(thread.subtitle)
                append('\n')
                append(thread.key)
                append('\n')
                append(thread.target)
                append('\n')
                append(if (thread.route == MessageRoute.DIRECT) "direct" else "channel")
                visibleMessages.forEach { message ->
                    append('\n')
                    append(message.senderName)
                    append('\n')
                    append(message.peerId.orEmpty())
                    append('\n')
                    append(message.channel.orEmpty())
                    append('\n')
                    append(message.text)
                }
            }.let(::normalize)
            terms.all(searchable::contains)
        }
    }

    private fun normalize(value: String): String =
        Normalizer.normalize(value, Normalizer.Form.NFKC).lowercase(Locale.ROOT)

    private val WHITESPACE = Regex("\\s+")
}
