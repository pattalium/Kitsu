package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import ptl.kitsu.app.model.Message

class MessagePresentationPolicyTest {
    @Test fun outgoingLifecycleUsesTheFirmwareStateInsteadOfStaticOutbound() {
        val expectations = mapOf(
            "queued" to "Queued",
            "sent" to "Sent",
            "delivered" to "Delivered",
            "unconfirmed" to "No ACK",
            "failed" to "Failed",
            "cancelled" to "Cancelled",
        )

        expectations.forEach { (state, badge) ->
            assertEquals(badge, MessagePresentationPolicy.present(message(state = state)).badge)
        }
    }

    @Test fun authenticatedDeliveryUsesOnlyTheProvenRouteRepeaterCount() {
        assertEquals(
            "Delivery acknowledged",
            MessagePresentationPolicy.present(message("delivered", repeaterCount = null)).detail,
        )
        assertEquals(
            "Delivered directly",
            MessagePresentationPolicy.present(message("delivered", repeaterCount = 0)).detail,
        )
        assertEquals(
            "Delivered via 1 repeater",
            MessagePresentationPolicy.present(message("delivered", repeaterCount = 1)).detail,
        )
        assertEquals(
            "Delivered via 3 repeaters",
            MessagePresentationPolicy.present(message("delivered", repeaterCount = 3)).detail,
        )
        assertFalse(
            MessagePresentationPolicy.present(message("delivered", repeaterCount = 3))
                .detail.contains("heard", ignoreCase = true),
        )
    }

    @Test fun missingAckRemainsUnknownRatherThanClaimingDeliveryFailure() {
        val detail = MessagePresentationPolicy.present(message("unconfirmed")).detail

        assertEquals("Transmitted, but no delivery ACK arrived. Delivery is unknown.", detail)
    }

    @Test fun channelSentExplainsThatNoDeliveryAckExists() {
        val detail = MessagePresentationPolicy.present(message("sent", channel = "0")).detail

        assertEquals("Transmitted by Kitsu; channels do not provide delivery ACKs.", detail)
    }

    @Test fun channelRepeatObservationsUseExactVisibleSingularAndPluralWording() {
        assertEquals(
            "Sent · no matching repeat recorded",
            MessagePresentationPolicy.conversationLine(message("sent", channel = "0", repeatCount = 0)),
        )
        assertEquals(
            "Sent · heard 1 repeat",
            MessagePresentationPolicy.conversationLine(message("sent", channel = "0", repeatCount = 1)),
        )
        assertEquals(
            "Sent · heard 7 repeats",
            MessagePresentationPolicy.conversationLine(message("sent", channel = "0", repeatCount = 7)),
        )
        assertEquals(
            "Sent · heard 255+ repeats",
            MessagePresentationPolicy.conversationLine(message("sent", channel = "0", repeatCount = 255)),
        )
    }

    @Test fun expandedAndAccessibilityCopyNeverConflatesLocalRepeatsWithDelivery() {
        val message = message("sent", channel = "0", repeatCount = 1)
        val expected = "Kitsu locally observed 1 matching rebroadcast packet copy. " +
            "Recipient reception remains unconfirmed."

        assertEquals(expected, MessagePresentationPolicy.present(message).detail)
        assertEquals(expected, MessagePresentationPolicy.accessibilityLine(message))
    }

    @Test fun zeroRepeatDetailDoesNotAssumeWhetherTheObservationWindowIsOpen() {
        val detail = MessagePresentationPolicy.present(
            message("sent", channel = "0", repeatCount = 0),
        ).detail

        assertEquals(
            "Kitsu has not recorded a matching rebroadcast copy for this message. " +
                "A repeater may still have forwarded it without Kitsu hearing the returned copy. " +
                "Recipient reception remains unconfirmed.",
            detail,
        )
    }

    @Test fun v4ObservationLifecycleDistinguishesListeningFromAClosedZeroResult() {
        val openZero = message(
            "sent",
            channel = "0",
            repeatCount = 0,
            repeatObservationOpen = true,
        )
        val openOne = message(
            "sent",
            channel = "0",
            repeatCount = 1,
            repeatObservationOpen = true,
        )
        val closedZero = message(
            "sent",
            channel = "0",
            repeatCount = 0,
            repeatObservationOpen = false,
        )

        assertEquals("Sent · listening for repeats", MessagePresentationPolicy.conversationLine(openZero))
        assertEquals("Sent · heard 1 repeat · listening", MessagePresentationPolicy.conversationLine(openOne))
        assertEquals("Sent · no matching repeat recorded", MessagePresentationPolicy.conversationLine(closedZero))
        assertEquals(
            "Kitsu is listening for a matching rebroadcast copy during its bounded observation window. " +
                "Recipient reception remains unconfirmed.",
            MessagePresentationPolicy.accessibilityLine(openZero),
        )
        assertEquals(
            "Kitsu's bounded observation window closed without recording a matching rebroadcast copy. " +
                "A repeater may still have forwarded the message without Kitsu hearing the returned copy. " +
                "Recipient reception remains unconfirmed.",
            MessagePresentationPolicy.accessibilityLine(closedZero),
        )
    }

    @Test fun saturatedChannelCountIsPresentedAsALowerBoundEverywhere() {
        val message = message("sent", channel = "0", repeatCount = 255)
        val expected = "Kitsu locally observed at least 255 matching rebroadcast packet copies. " +
            "Recipient reception remains unconfirmed."

        assertEquals("Sent · heard 255+ repeats", MessagePresentationPolicy.conversationLine(message))
        assertEquals(expected, MessagePresentationPolicy.present(message).detail)
        assertEquals(expected, MessagePresentationPolicy.accessibilityLine(message))
    }

    @Test fun inboundDeviceUnreadEvidenceIsNamedNarrowly() {
        val inbound = message("received", direction = "inbound", unreadOnKitsu = true)
        val presentation = MessagePresentationPolicy.present(inbound)

        assertEquals("Unread", presentation.badge)
        assertEquals("Incoming from the mesh • Unread on physical Kitsu", presentation.detail)
    }

    @Test fun inboundZeroRouteCountDescribesOnlyTheCopyReceivedByThisKitsu() {
        val inbound = message(
            "received",
            direction = "inbound",
            repeaterCount = 0,
            route = "flood",
        )

        assertEquals(
            "Received directly by this Kitsu • Received",
            MessagePresentationPolicy.present(inbound).detail,
        )
        assertEquals("Direct to Kitsu", MessagePresentationPolicy.inboundCopyRouteLine(inbound))
    }

    @Test fun inboundPositiveRouteCountsPreserveTheProvenRouteWording() {
        val one = message(
            "received",
            direction = "inbound",
            repeaterCount = 1,
            route = "flood",
        )
        val many = message(
            "received",
            direction = "inbound",
            repeaterCount = 3,
            route = "flood",
        )

        assertEquals("Incoming via 1 route repeater • Received", MessagePresentationPolicy.present(one).detail)
        assertEquals("via 1 route repeater", MessagePresentationPolicy.inboundCopyRouteLine(one))
        assertEquals("Incoming via 3 route repeaters • Received", MessagePresentationPolicy.present(many).detail)
        assertEquals("via 3 route repeaters", MessagePresentationPolicy.inboundCopyRouteLine(many))
    }

    private fun message(
        state: String,
        direction: String = "outbound",
        channel: String? = null,
        repeaterCount: Int? = null,
        repeatCount: Int? = null,
        repeatObservationOpen: Boolean? = null,
        unreadOnKitsu: Boolean? = null,
        route: String? = null,
    ) = Message(
        id = "1",
        cursor = "1",
        direction = direction,
        channel = channel,
        text = "hello",
        state = state,
        occurredAt = 1,
        repeaterCount = repeaterCount,
        repeatCount = repeatCount,
        repeatObservationOpen = repeatObservationOpen,
        unreadOnKitsu = unreadOnKitsu,
        route = route,
    )
}
