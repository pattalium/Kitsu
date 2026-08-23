package ptl.kitsu.app.repository

import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.transport.TransportException

internal data class LoadedMessageSnapshot(
    val items: List<Message>,
    val protocolVersion: Int,
    val journalSession: String?,
    val journalRevision: String?,
)

/** Drains the bounded firmware journal without ever committing a partial page set. */
internal object MessageSnapshotLoader {
    const val PAGE_SIZE = 8
    const val MAX_MESSAGES = 24
    private const val MAX_PAGES_PER_ATTEMPT = 8
    private const val MAX_ATTEMPTS = 2
    private const val UINT32_MODULUS = 0x1_0000_0000L
    private const val UINT32_HALF_RANGE = 0x8000_0000L

    suspend fun load(
        fetch: suspend (after: String?, limit: Int) -> MessagePage,
    ): List<Message> = loadSnapshot(fetch).items

    suspend fun loadSnapshot(
        fetch: suspend (after: String?, limit: Int) -> MessagePage,
    ): LoadedMessageSnapshot {
        repeat(MAX_ATTEMPTS) { attempt ->
            when (val result = loadAttempt(fetch)) {
                is Attempt.Complete -> return LoadedMessageSnapshot(
                    items = result.items,
                    protocolVersion = result.protocolVersion,
                    journalSession = result.journalSession,
                    journalRevision = result.journalRevision,
                )
                Attempt.Restart -> if (attempt + 1 == MAX_ATTEMPTS) {
                    throw TransportException("messages_snapshot_changed")
                }
            }
        }
        throw TransportException("messages_snapshot_changed")
    }

    private suspend fun loadAttempt(
        fetch: suspend (after: String?, limit: Int) -> MessagePage,
    ): Attempt {
        var after: String? = OwnerCursorPolicy.messagesAfter()
        var journalSession: String? = null
        var journalRevision: String? = null
        var protocolVersion: Int? = null
        val messages = LinkedHashMap<String, Message>(MAX_MESSAGES)

        repeat(MAX_PAGES_PER_ATTEMPT) {
            val page = fetch(after, PAGE_SIZE)
            if (page.cursorExpired) return Attempt.Restart
            if (page.items.size > PAGE_SIZE) {
                throw TransportException("messages_snapshot_page_overflow")
            }

            if (page.protocolVersion !in 1..4) {
                throw TransportException("messages_snapshot_protocol_invalid")
            }
            if (protocolVersion == null) {
                protocolVersion = page.protocolVersion
            } else if (protocolVersion != page.protocolVersion) {
                return Attempt.Restart
            }

            val pageJournalSession = page.journalSession
            val pageJournalRevision = page.journalRevision
            if (page.protocolVersion >= 2 && pageJournalSession == null) {
                throw TransportException("messages_snapshot_session_missing")
            }
            if (page.protocolVersion >= 2 && pageJournalRevision == null) {
                throw TransportException("messages_snapshot_revision_missing")
            }
            if (page.protocolVersion == 1 &&
                (pageJournalSession != null || pageJournalRevision != null)
            ) {
                throw TransportException("messages_snapshot_revision_unexpected")
            }
            if (pageJournalSession != null && parseUint32(pageJournalSession) == null) {
                throw TransportException("messages_snapshot_session_invalid")
            }
            if (pageJournalRevision != null && parseUint32(pageJournalRevision, allowZero = true) == null) {
                throw TransportException("messages_snapshot_revision_invalid")
            }
            if (journalSession == null) {
                journalSession = pageJournalSession
            } else if (pageJournalSession != journalSession) {
                return Attempt.Restart
            }
            if (journalRevision == null) {
                journalRevision = pageJournalRevision
            } else if (pageJournalRevision != journalRevision) {
                return Attempt.Restart
            }

            var previousItemId = after
            page.items.forEach { message ->
                if (parseUint32(message.id) == null || parseUint32(message.revision) == null) {
                    throw TransportException("messages_snapshot_item_invalid")
                }
                if (messages.containsKey(message.id)) {
                    throw TransportException("messages_snapshot_duplicate")
                }
                if (previousItemId != null && !cursorIsForward(previousItemId, message.id)) {
                    throw TransportException("messages_snapshot_order_invalid")
                }
                messages[message.id] = message
                if (messages.size > MAX_MESSAGES) {
                    throw TransportException("messages_snapshot_overflow")
                }
                previousItemId = message.id
            }

            if (page.items.isNotEmpty() && page.cursor != page.items.last().id) {
                throw TransportException("messages_snapshot_cursor_binding")
            }

            if (!page.hasMore) return Attempt.Complete(
                items = messages.values.toList(),
                protocolVersion = checkNotNull(protocolVersion),
                journalSession = journalSession,
                journalRevision = journalRevision,
            )
            val cursor = page.cursor
                ?: throw TransportException("messages_snapshot_cursor_missing")
            if (page.items.isEmpty() || !cursorIsForward(after, cursor)) {
                throw TransportException("messages_snapshot_stalled")
            }
            after = cursor
        }
        throw TransportException("messages_snapshot_page_limit")
    }

    private fun cursorIsForward(previous: String?, current: String): Boolean {
        val currentValue = parseUint32(current) ?: return false
        val previousValue = previous?.let(::parseUint32) ?: return true
        val distance = (currentValue - previousValue + UINT32_MODULUS) % UINT32_MODULUS
        return distance in 1 until UINT32_HALF_RANGE
    }

    private fun parseUint32(value: String, allowZero: Boolean = false): Long? = value.toLongOrNull()
        ?.takeIf {
            it in (if (allowZero) 0L else 1L) until UINT32_MODULUS &&
                value == it.toString()
        }

    private sealed interface Attempt {
        data class Complete(
            val items: List<Message>,
            val protocolVersion: Int,
            val journalSession: String?,
            val journalRevision: String?,
        ) : Attempt
        data object Restart : Attempt
    }
}
