package ptl.kitsu.app.repository

import ptl.kitsu.app.cache.CachePolicy
import ptl.kitsu.app.model.Message

/** Merges the live mutable ring into bounded, encrypted on-phone conversation history. */
internal object MessageSnapshotPolicy {
    /** Legacy v1 has no boot namespace, so cross-refresh/reboot merging is unsafe. */
    fun fullRing(items: List<Message>): List<Message> {
        require(items.distinctBy(Message::id).size == items.size) {
            "duplicate_message_id"
        }
        return items.takeLast(CachePolicy.MAX_MESSAGES)
    }

    fun merge(
        previous: List<Message>,
        snapshot: LoadedMessageSnapshot,
    ): List<Message> {
        if (snapshot.protocolVersion == 1) return fullRing(snapshot.items)
        require(snapshot.protocolVersion in 2..4) { "unsupported_message_protocol" }
        val session = requireNotNull(snapshot.journalSession) { "message_session_required" }
        require(snapshot.items.all { it.journalSession == session }) {
            "message_session_mismatch"
        }
        require(snapshot.items.distinctBy(Message::id).size == snapshot.items.size) {
            "duplicate_message_id"
        }

        val liveIds = snapshot.items.mapTo(hashSetOf(), Message::id)
        val merged = linkedMapOf<String, Message>()
        previous.asSequence()
            // Pre-v2 records have no collision-safe namespace and are deliberately
            // replaced once a trusted v2 snapshot is available.
            .filter { it.journalSession != null }
            .forEach { message ->
                val noLongerUnreadOnDevice = message.journalSession != session || message.id !in liveIds
                merged[message.stableJournalKey()] = if (noLongerUnreadOnDevice && message.unreadOnKitsu == true) {
                    message.copy(unreadOnKitsu = false)
                } else {
                    message
                }
            }
        snapshot.items.forEach { message -> merged[message.stableJournalKey()] = message }
        return merged.values.toList().takeLast(CachePolicy.MAX_MESSAGES)
    }
}

internal fun Message.stableJournalKey(): String =
    journalSession?.let { "$it:$id" } ?: "legacy:$id"
