package ptl.kitsu.app

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.ui.KitsuTheme
import ptl.kitsu.app.ui.KitsuThemePreference
import ptl.kitsu.app.ui.MessageCard

@RunWith(AndroidJUnit4::class)
class MessageActionsUiTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun outboundMessageNeverExposesModerationActions() {
        show(message(direction = "outbound", peerId = "peer-alpha"), onBlock = {})

        compose.onNodeWithTag(ACTIONS_TAG).assertDoesNotExist()
        moderationLabels().forEach { compose.onNodeWithText(it).assertDoesNotExist() }
    }

    @Test
    fun inboundChannelOffersOnlyReportMessage() {
        show(message(direction = "inbound", peerId = null, channel = "0"), onBlock = null)

        moderationLabels().forEach { compose.onNodeWithText(it).assertDoesNotExist() }
        compose.onNodeWithTag(ACTIONS_TAG).assertIsDisplayed().performClick()
        compose.onNodeWithText("Report message").assertIsDisplayed()
        compose.onNodeWithText("Report sender").assertDoesNotExist()
        compose.onNodeWithText("Block sender").assertDoesNotExist()
    }

    @Test
    fun inboundDirectKeepsActionsInsideCompactOverflow() {
        show(message(direction = "inbound", peerId = "peer-alpha"), onBlock = {})

        moderationLabels().forEach { compose.onNodeWithText(it).assertDoesNotExist() }
        compose.onNodeWithTag(ACTIONS_TAG).assertIsDisplayed().performClick()
        moderationLabels().forEach { compose.onNodeWithText(it).assertIsDisplayed() }
    }

    @Test
    fun directHeadingUsesKnownPeerNameWithoutRenderingTheRawIdentity() {
        val peerId = "A".repeat(43)
        show(
            message = message(direction = "inbound", peerId = peerId),
            onBlock = {},
            peerLabel = "Copper Fox",
        )

        compose.onNodeWithText("Copper Fox").assertIsDisplayed()
        compose.onNodeWithText(peerId).assertDoesNotExist()
    }

    @Test
    fun prominentOutboundBadgeTracksDeliveryLifecycle() {
        show(message(direction = "outbound", peerId = "peer-alpha", state = "unconfirmed"), onBlock = null)

        compose.onNodeWithText("No ACK · delivery unknown").assertIsDisplayed()
        compose.onNodeWithText("Outbound").assertDoesNotExist()
    }

    @Test
    fun zeroHopProvenAckRendersDirectDeliveryTruthfully() {
        show(
            message("outbound", "peer-alpha", state = "delivered", repeaterCount = 0),
            onBlock = null,
        )
        compose.onNodeWithText("Delivered directly").assertIsDisplayed()
    }

    @Test
    fun provenRepeaterRouteCountRendersDeliveryTruthfully() {
        show(
            message("outbound", "peer-alpha", state = "delivered", repeaterCount = 2),
            onBlock = null,
        )
        compose.onNodeWithText("Delivered via 2 repeaters").assertIsDisplayed()
    }

    @Test
    fun outboundChannelShowsOpenLocalRepeatObservationWithoutClaimingRecipientDelivery() {
        show(
            message(
                direction = "outbound",
                peerId = null,
                channel = "0",
                state = "sent",
                repeatCount = 1,
                repeatObservationOpen = true,
            ),
            onBlock = null,
        )

        compose.onNodeWithText("Sent · heard 1 repeat · listening").assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Kitsu locally observed 1 matching rebroadcast packet copy",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Recipient reception remains unconfirmed",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithText("Delivered", substring = true).assertDoesNotExist()
    }

    @Test
    fun outboundChannelClosedZeroDoesNotClaimARepeaterOrRecipientFailure() {
        show(
            message(
                direction = "outbound",
                peerId = null,
                channel = "0",
                state = "sent",
                repeatCount = 0,
                repeatObservationOpen = false,
            ),
            onBlock = null,
        )

        compose.onNodeWithText("Sent · no matching repeat recorded").assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "bounded observation window closed without recording a matching rebroadcast copy",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "A repeater may still have forwarded the message",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Recipient reception remains unconfirmed",
            substring = true,
        ).assertIsDisplayed()
    }

    @Test
    fun incomingChannelShowsSenderAndPhysicalKitsuUnreadEvidence() {
        show(
            message(
                direction = "inbound",
                peerId = null,
                channel = "0",
                state = "received",
                senderName = "Alice",
                unreadOnKitsu = true,
                journalSession = "15",
            ),
            onBlock = null,
        )

        compose.onNodeWithText("Alice · unverified").assertIsDisplayed()
        compose.onNodeWithText("Unread on Kitsu", substring = true).assertIsDisplayed()
    }

    @Test
    fun incomingZeroRouteCountIsDirectToThisKitsuAndKeepsLastHopEvidence() {
        show(
            message(
                direction = "inbound",
                peerId = null,
                channel = "0",
                state = "received",
                senderName = "Alice",
                repeaterCount = 0,
                route = "flood",
                rssiDbm = -51.0,
                snrDb = 13.0,
                journalSession = "15",
            ),
            onBlock = null,
        )

        compose.onNodeWithText("Direct to Kitsu", substring = true).assertIsDisplayed()
        compose.onNodeWithText("no route repeaters", substring = true).assertDoesNotExist()
        compose.onNodeWithText("-51 dBm last hop", substring = true).assertIsDisplayed()
        compose.onNodeWithContentDescription("Direct to Kitsu", substring = true).assertIsDisplayed()
        compose.onNodeWithContentDescription("-51 dBm last hop", substring = true).assertIsDisplayed()
    }

    private fun show(
        message: Message,
        onBlock: (() -> Unit)?,
        peerLabel: String? = null,
    ) {
        compose.setContent {
            KitsuTheme(KitsuThemePreference.DARK) {
                MessageCard(
                    message = message,
                    peerLabel = peerLabel,
                    updateBusy = false,
                    onReport = {},
                    onBlock = onBlock,
                )
            }
        }
    }

    private fun message(
        direction: String,
        peerId: String?,
        channel: String? = null,
        state: String = "delivered",
        senderName: String = "",
        unreadOnKitsu: Boolean? = null,
        repeaterCount: Int? = null,
        repeatCount: Int? = null,
        repeatObservationOpen: Boolean? = null,
        route: String? = null,
        rssiDbm: Double? = null,
        snrDb: Double? = null,
        journalSession: String? = null,
    ) = Message(
        id = "message-1",
        cursor = "cursor-1",
        direction = direction,
        peerId = peerId,
        channel = channel,
        text = "Hello from the mesh",
        state = state,
        occurredAt = 1,
        senderName = senderName,
        unreadOnKitsu = unreadOnKitsu,
        repeaterCount = repeaterCount,
        repeatCount = repeatCount,
        repeatObservationOpen = repeatObservationOpen,
        route = route,
        rssiDbm = rssiDbm,
        snrDb = snrDb,
        journalSession = journalSession,
    )

    private fun moderationLabels() = listOf("Report message", "Report sender", "Block sender")

    private companion object {
        const val ACTIONS_TAG = "message-actions-legacy:message-1"
    }
}
