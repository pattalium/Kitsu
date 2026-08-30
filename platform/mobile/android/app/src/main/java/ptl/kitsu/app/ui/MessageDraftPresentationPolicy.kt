package ptl.kitsu.app.ui

import java.util.Locale
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.model.Peer

/** Keeps a valid unsent draft reachable even when its conversation has no journal row yet. */
internal object MessageDraftPresentationPolicy {
    fun includeDraftThreads(
        threads: List<MessageThread>,
        drafts: Map<String, String>,
        peers: List<Peer>,
        channels: List<MeshChannel>,
        blockedPeerIds: Set<String>,
    ): List<MessageThread> {
        val existing = threads.mapTo(mutableSetOf(), MessageThread::key)
        val peerNames = peers.associate { it.id to it.name.trim() }
        val channelsBySlot = channels.filter { it.configured }.associateBy(MeshChannel::slot)
        val draftOnly = drafts.keys.mapNotNull { key ->
            if (!existing.add(key)) return@mapNotNull null
            when {
                key.startsWith("direct:") -> {
                    val peerId = key.removePrefix("direct:")
                    if (peerId in blockedPeerIds || !MeshPeerKeyPolicy.isCanonicalBase64Url(peerId)) {
                        return@mapNotNull null
                    }
                    MessageThread(
                        key = key,
                        route = MessageRoute.DIRECT,
                        target = peerId,
                        title = peerNames[peerId]?.takeIf(String::isNotBlank)
                            ?: MessageComposerPolicy.compactReference(peerId),
                        subtitle = "Direct message · ${MessageComposerPolicy.compactReference(peerId)}",
                        messages = emptyList(),
                        unreadCount = 0,
                        latestJournalPosition = -1,
                    )
                }
                key.startsWith("channel:") -> {
                    val slot = key.removePrefix("channel:").toIntOrNull()
                        ?.takeIf { it in 0..3 } ?: return@mapNotNull null
                    val channel = channelsBySlot[slot]
                    val title = channel?.name?.trim().orEmpty().ifBlank { "Channel $slot" }
                    val routing = channel?.let {
                        ChannelRoutingPresentationPolicy.present(it.regionScope).label
                    } ?: "Routing unavailable"
                    MessageThread(
                        key = key,
                        route = MessageRoute.CHANNEL,
                        target = slot.toString(),
                        title = title,
                        subtitle = "Channel messages · slot $slot · $routing",
                        messages = emptyList(),
                        unreadCount = 0,
                        latestJournalPosition = -1,
                        channelRegionScope = channel?.regionScope,
                        channelRoutingKnown = channel != null,
                    )
                }
                else -> null
            }
        }.sortedBy { it.title.lowercase(Locale.ROOT) }
        return threads + draftOnly
    }
}
