package ptl.kitsu.app.ui

import java.time.Instant
import java.time.ZoneId
import java.time.LocalDate
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.Locale
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.ChannelRegionScope
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.Peer

internal data class MessageThread(
    val key: String,
    val route: MessageRoute,
    val target: String,
    val title: String,
    val subtitle: String,
    val messages: List<Message>,
    val unreadCount: Int,
    internal val latestJournalPosition: Int,
    val channelRegionScope: ChannelRegionScope? = null,
    val channelRoutingKnown: Boolean = false,
) {
    val latestMessage: Message? get() = messages.lastOrNull()
}

internal sealed interface ConversationTimelineItem {
    val stableKey: String

    data class DateSeparator(
        val label: String,
        override val stableKey: String,
    ) : ConversationTimelineItem

    data class Bubble(val message: Message) : ConversationTimelineItem {
        override val stableKey: String = "message:${message.uiStableJournalKey()}"
    }
}

/** Builds stable route/identity conversations from the firmware's journal-order snapshot. */
internal object MessageThreadPolicy {
    private const val DIRECT_PREFIX = "direct:"
    private const val CHANNEL_PREFIX = "channel:"

    fun directKey(peerId: String): String = "$DIRECT_PREFIX$peerId"
    fun channelKey(slot: Int): String = "$CHANNEL_PREFIX$slot"

    fun build(
        messages: List<Message>,
        peers: List<Peer>,
        channels: List<MeshChannel>,
        blockedPeerIds: Set<String>,
        currentJournalSession: String? = null,
    ): List<MessageThread> {
        val peerNames = peers
            .asSequence()
            .filter { it.id !in blockedPeerIds }
            .associate { it.id to it.name.trim() }
        val channelMetadata = channels
            .asSequence()
            .filter { it.configured && it.slot in 0..3 }
            .associateBy(MeshChannel::slot)
        val journalPositions = messages.mapIndexed { index, message ->
            message.uiStableJournalKey() to index
        }.toMap()
        val grouped = linkedMapOf<String, MutableList<Message>>()

        channelMetadata.keys.sorted().forEach { grouped[channelKey(it)] = mutableListOf() }
        messages.forEach { message ->
            val key = conversationKey(message) ?: return@forEach
            if (message.peerId in blockedPeerIds) return@forEach
            grouped.getOrPut(key) { mutableListOf() } += message
        }

        return grouped.mapNotNull { (key, threadMessages) ->
            when {
                key.startsWith(CHANNEL_PREFIX) -> {
                    val slot = key.removePrefix(CHANNEL_PREFIX).toIntOrNull() ?: return@mapNotNull null
                    val channel = channelMetadata[slot]
                    val configuredName = channel?.name?.trim().orEmpty()
                    val title = configuredName.ifBlank { "Channel $slot" }
                    val routingLabel = channel?.let {
                        ChannelRoutingPresentationPolicy.present(it.regionScope).label
                    } ?: "Routing unavailable"
                    MessageThread(
                        key = key,
                        route = MessageRoute.CHANNEL,
                        target = slot.toString(),
                        title = title,
                        subtitle = "Channel messages · slot $slot · $routingLabel",
                        messages = threadMessages.toList(),
                        unreadCount = threadMessages.count {
                            it.isUnreadInbound(currentJournalSession)
                        },
                        latestJournalPosition = threadMessages.lastOrNull()
                            ?.let { journalPositions[it.uiStableJournalKey()] }
                            ?: -1,
                        channelRegionScope = channel?.regionScope,
                        channelRoutingKnown = channel != null,
                    )
                }
                key.startsWith(DIRECT_PREFIX) -> {
                    val peerId = key.removePrefix(DIRECT_PREFIX)
                    if (peerId in blockedPeerIds) return@mapNotNull null
                    val senderName = threadMessages.asReversed()
                        .firstNotNullOfOrNull { message ->
                            message.senderName.trim().takeIf {
                                it.isNotEmpty() &&
                                    message.direction.equals("inbound", ignoreCase = true)
                            }
                        }
                    val title = peerNames[peerId]
                        ?.takeIf(String::isNotBlank)
                        ?: senderName
                        ?: MessageComposerPolicy.compactReference(peerId)
                    MessageThread(
                        key = key,
                        route = MessageRoute.DIRECT,
                        target = peerId,
                        title = title,
                        subtitle = "Direct message · ${MessageComposerPolicy.compactReference(peerId)}",
                        messages = threadMessages.toList(),
                        unreadCount = threadMessages.count {
                            it.isUnreadInbound(currentJournalSession)
                        },
                        latestJournalPosition = threadMessages.lastOrNull()
                            ?.let { journalPositions[it.uiStableJournalKey()] }
                            ?: -1,
                    )
                }
                else -> null
            }
        }.sortedWith(
            compareByDescending<MessageThread> { it.latestMessage != null }
                .thenByDescending(MessageThread::latestJournalPosition)
                .thenBy { it.title.lowercase(Locale.ROOT) },
        )
    }

    fun conversationKey(message: Message): String? = when {
        message.channel != null -> message.channel.toIntOrNull()
            ?.takeIf { it in 0..3 }
            ?.let(::channelKey)
        message.peerId != null -> directKey(message.peerId)
        else -> null
    }

    fun timeline(
        messages: List<Message>,
        zoneId: ZoneId = ZoneId.systemDefault(),
        locale: Locale = Locale.getDefault(),
    ): List<ConversationTimelineItem> = buildList {
        var previousDayKey: String? = null
        messages.forEachIndexed { index, message ->
            val dayKey = MessageTimePolicy.dayKey(message.occurredAt, zoneId)
            if (dayKey != previousDayKey) {
                add(
                    ConversationTimelineItem.DateSeparator(
                        label = MessageTimePolicy.dateLabel(message.occurredAt, zoneId, locale),
                        stableKey = "date:${message.uiStableJournalKey()}:$index:$dayKey",
                    ),
                )
                previousDayKey = dayKey
            }
            add(ConversationTimelineItem.Bubble(message))
        }
    }

    private fun Message.isUnreadInbound(currentJournalSession: String?): Boolean =
        currentJournalSession != null && journalSession == currentJournalSession &&
            direction.equals("inbound", ignoreCase = true) && unreadOnKitsu == true
}

internal fun Message.uiStableJournalKey(): String =
    journalSession?.let { "$it:$id" } ?: "legacy:$id"

internal object MessageTimePolicy {
    // Remote/channel timestamps can be untrusted. Only render modern, plausible
    // calendar values; journal order remains authoritative regardless of time.
    private const val MIN_PLAUSIBLE_EPOCH_SECONDS = 1_704_067_200L // 2024-01-01 UTC
    private const val MAX_PLAUSIBLE_EPOCH_SECONDS = 4_102_444_799L // before 2100-01-01 UTC

    fun isAvailable(epochSeconds: Long): Boolean =
        epochSeconds in MIN_PLAUSIBLE_EPOCH_SECONDS..MAX_PLAUSIBLE_EPOCH_SECONDS

    fun dayKey(epochSeconds: Long, zoneId: ZoneId = ZoneId.systemDefault()): String =
        if (isAvailable(epochSeconds)) {
            Instant.ofEpochSecond(epochSeconds).atZone(zoneId).toLocalDate().toString()
        } else {
            "unavailable"
        }

    fun dateLabel(
        epochSeconds: Long,
        zoneId: ZoneId = ZoneId.systemDefault(),
        locale: Locale = Locale.getDefault(),
        today: LocalDate = LocalDate.now(zoneId),
    ): String {
        if (!isAvailable(epochSeconds)) return "Date unavailable"
        val date = Instant.ofEpochSecond(epochSeconds).atZone(zoneId).toLocalDate()
        return when (date) {
            today -> "Today"
            today.minusDays(1) -> "Yesterday"
            else -> DateTimeFormatter.ofLocalizedDate(FormatStyle.MEDIUM)
                .withLocale(locale)
                .format(date)
        }
    }

    fun timeLabel(
        epochSeconds: Long,
        zoneId: ZoneId = ZoneId.systemDefault(),
        locale: Locale = Locale.getDefault(),
    ): String =
        if (isAvailable(epochSeconds)) {
            DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT)
                .withLocale(locale)
                .format(Instant.ofEpochSecond(epochSeconds).atZone(zoneId))
        } else {
            "Time unavailable"
        }
}
