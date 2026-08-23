package ptl.kitsu.app.ui

import ptl.kitsu.app.model.Message

internal data class MessagePresentation(
    val badge: String,
    val detail: String,
    val tone: StatusTone,
)

/** Turns firmware evidence into labels without inferring unsupported delivery claims. */
internal object MessagePresentationPolicy {
    fun present(message: Message, peers: List<ptl.kitsu.app.model.Peer> = emptyList()): MessagePresentation {
        if (message.direction.equals("inbound", ignoreCase = true)) {
            val route = when (val repeaters = message.repeaterCount) {
                0 -> "Received directly by this Kitsu"
                1 -> "Incoming via 1 route repeater"
                in 2..63 -> "Incoming via $repeaters route repeaters"
                else -> "Incoming from the mesh"
            }
            val unread = if (message.unreadOnKitsu == true) {
                "$route • Unread on physical Kitsu"
            } else {
                "$route • Received"
            }
            return MessagePresentation(
                badge = if (message.unreadOnKitsu == true) "Unread" else "Incoming",
                detail = unread,
                tone = StatusTone.POSITIVE,
            )
        }

        return when (message.state) {
            "queued" -> MessagePresentation(
                "Queued",
                "Queued on Kitsu; waiting for radio transmission.",
                StatusTone.ACTIVE,
            )
            "sent" -> MessagePresentation(
                "Sent",
                if (message.channel != null) {
                    message.validLocalRepeatCount()?.let { count ->
                        localRepeatEvidenceDetail(message, count, peers)
                    }
                        ?: "Transmitted by Kitsu; channels do not provide delivery ACKs."
                } else {
                    "Transmitted by Kitsu; waiting for a delivery ACK."
                },
                StatusTone.ACTIVE,
            )
            "delivered" -> MessagePresentation(
                "Delivered",
                when (val repeaters = message.repeaterCount) {
                    0 -> "Delivered directly"
                    1 -> "Delivered via 1 repeater"
                    in 2..63 -> "Delivered via $repeaters repeaters"
                    else -> "Delivery acknowledged"
                },
                StatusTone.POSITIVE,
            )
            "unconfirmed" -> MessagePresentation(
                "No ACK",
                "Transmitted, but no delivery ACK arrived. Delivery is unknown.",
                StatusTone.ACTIVE,
            )
            "failed" -> MessagePresentation(
                "Failed",
                "Kitsu did not complete the radio transmission.",
                StatusTone.NEGATIVE,
            )
            "cancelled" -> MessagePresentation(
                "Cancelled",
                "Cancelled before radio delivery could be confirmed.",
                StatusTone.NEUTRAL,
            )
            else -> MessagePresentation(
                message.state.humanized(),
                "Outgoing message status reported by Kitsu.",
                StatusTone.NEUTRAL,
            )
        }
    }

    /** Compact chat metadata; never implies that a human read a message. */
    fun conversationLine(message: Message): String = when (message.state) {
        "queued" -> "Queued"
        "sent" -> if (message.channel != null) {
            message.validLocalRepeatCount()?.let { count ->
                localRepeatConversationLine(count, message.repeatObservationOpen)
            }
                ?: "Sent · channel reception unconfirmed"
        } else {
            "Sent · waiting for ACK"
        }
        "delivered" -> when (val repeaters = message.repeaterCount) {
            0 -> "Delivered directly"
            1 -> "Delivered via 1 repeater"
            in 2..63 -> "Delivered via $repeaters repeaters"
            else -> "Delivery acknowledged"
        }
        "unconfirmed" -> "No ACK · delivery unknown"
        "failed" -> "Failed"
        "cancelled" -> "Cancelled"
        else -> message.state.humanized()
    }

    /** Full status used by accessibility and any expanded lifecycle surface. */
    fun accessibilityLine(
        message: Message,
        peers: List<ptl.kitsu.app.model.Peer> = emptyList(),
    ): String = present(message, peers).detail

    /** Compact route evidence for one inbound flood copy; this is not a network-wide relay claim. */
    fun inboundCopyRouteLine(message: Message): String? {
        if (!message.direction.equals("inbound", ignoreCase = true) || message.route != "flood") {
            return null
        }
        return when (val repeaters = message.repeaterCount) {
            0 -> "Direct to Kitsu"
            1 -> "via 1 route repeater"
            in 2..63 -> "via $repeaters route repeaters"
            else -> null
        }
    }

    private fun Message.validLocalRepeatCount(): Int? = repeatCount?.takeIf { it in 0..255 }

    private fun localRepeatConversationLine(count: Int, observationOpen: Boolean?): String = when {
        observationOpen == true && count == 0 -> "Sent · listening for repeats"
        observationOpen == true && count == 1 -> "Sent · heard 1 repeat · listening"
        observationOpen == true && count == 255 -> "Sent · heard 255+ repeats · listening"
        observationOpen == true -> "Sent · heard $count repeats · listening"
        count == 0 -> "Sent · no matching repeat recorded"
        count == 1 -> "Sent · heard 1 repeat"
        count == 255 -> "Sent · heard 255+ repeats"
        else -> "Sent · heard $count repeats"
    }

    private fun localRepeatEvidenceDetail(
        message: Message,
        count: Int,
        peers: List<ptl.kitsu.app.model.Peer>,
    ): String {
        val observationOpen = message.repeatObservationOpen
        if (count == 0) {
            return when (observationOpen) {
                true -> "Kitsu is listening for a matching rebroadcast copy during its bounded " +
                    "observation window. Recipient reception remains unconfirmed."
                false -> "Kitsu's bounded observation window closed without recording a matching " +
                    "rebroadcast copy. A repeater may still have forwarded the message without Kitsu " +
                    "hearing the returned copy. Recipient reception remains unconfirmed."
                null -> "Kitsu has not recorded a matching rebroadcast copy for this message. " +
                    "A repeater may still have forwarded it without Kitsu hearing the returned copy. " +
                    "Recipient reception remains unconfirmed."
            }
        }
        val observed = if (count == 255) {
            "at least 255 matching rebroadcast packet copies"
        } else {
            "$count matching rebroadcast ${if (count == 1) "packet copy" else "packet copies"}"
        }
        val lifecycle = when (observationOpen) {
            true -> "Kitsu locally observed $observed and is still listening during its bounded observation window."
            false -> "Kitsu's bounded observation window closed after locally observing $observed."
            null -> "Kitsu locally observed $observed."
        }
        val sourceDetail = RepeatSourcePresentationPolicy.detail(message, peers)
        return buildString {
            append(lifecycle)
            if (sourceDetail != null) append(" $sourceDetail")
            append(" Recipient reception remains unconfirmed.")
        }
    }
}
