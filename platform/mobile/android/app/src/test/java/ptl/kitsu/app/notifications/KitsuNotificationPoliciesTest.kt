package ptl.kitsu.app.notifications

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionBond
import ptl.kitsu.app.model.CompanionCheckIn
import ptl.kitsu.app.model.CompanionComfort
import ptl.kitsu.app.model.CompanionComfortKind
import ptl.kitsu.app.model.CompanionDailyGoal
import ptl.kitsu.app.model.CompanionDevelopment
import ptl.kitsu.app.model.CompanionGoalKind
import ptl.kitsu.app.model.CompanionMood
import ptl.kitsu.app.model.CompanionPersonalBests
import ptl.kitsu.app.model.CompanionPersonality
import ptl.kitsu.app.model.CompanionPreferences
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.CompanionQuickAction
import ptl.kitsu.app.model.CompanionQuietHours
import ptl.kitsu.app.model.CompanionRequest
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.CompanionSettings
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.PET_FEATURE_SCHEMA_VERSION
import ptl.kitsu.app.model.PetPersonality
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.transport.ConnectionMode

class KitsuNotificationPoliciesTest {
    private val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    private val address = "02:00:00:00:00:32"

    @Test fun directJournalColdSnapshotIsBaselineThenOnlyNewUnblockedDirectRowsEmit() {
        val first = directMessage("1")
        val baseline = DirectMessageNotificationPolicy.evaluate(
            DirectMessageNotificationCursor(),
            directSnapshot(listOf(first)),
        )
        assertTrue(baseline.newMessages.isEmpty())

        val second = directMessage("2")
        val fresh = DirectMessageNotificationPolicy.evaluate(
            baseline.cursor,
            directSnapshot(listOf(first, second)),
        )
        assertEquals(listOf("2"), fresh.newMessages.map { it.id })

        val repeated = DirectMessageNotificationPolicy.evaluate(
            fresh.cursor,
            directSnapshot(listOf(first, second)),
        )
        assertTrue(repeated.newMessages.isEmpty())

        val blockedThird = directMessage("3")
        val blocked = DirectMessageNotificationPolicy.evaluate(
            repeated.cursor,
            directSnapshot(listOf(first, second, blockedThird), blocked = setOf(peer)),
        )
        assertTrue(blocked.newMessages.isEmpty())
        val laterUnblocked = DirectMessageNotificationPolicy.evaluate(
            blocked.cursor,
            directSnapshot(listOf(first, second, blockedThird)),
        )
        assertTrue(laterUnblocked.newMessages.isEmpty())
    }

    @Test fun directJournalDisconnectRetainsCursorButDeviceOrBootSessionChangeRebaselines() {
        val first = DirectMessageNotificationPolicy.evaluate(
            DirectMessageNotificationCursor(),
            directSnapshot(listOf(directMessage("1"))),
        )
        val disconnected = DirectMessageNotificationPolicy.evaluate(
            first.cursor,
            directSnapshot(listOf(directMessage("1"), directMessage("2"))).copy(connected = false),
        )
        assertEquals(first.cursor, disconnected.cursor)

        val otherDevice = DirectMessageNotificationPolicy.evaluate(
            disconnected.cursor,
            directSnapshot(listOf(directMessage("1"), directMessage("2"))).copy(
                deviceAddress = "02:00:00:00:00:33",
            ),
        )
        assertTrue(otherDevice.newMessages.isEmpty())
        val newSession = DirectMessageNotificationPolicy.evaluate(
            otherDevice.cursor,
            directSnapshot(listOf(directMessage("9", session = "next"))).copy(
                deviceAddress = "02:00:00:00:00:33",
                journalSession = "next",
            ),
        )
        assertTrue(newSession.newMessages.isEmpty())
    }

    @Test fun petPolicyWaitsForLiveStateBaselinesColdStateAndUsesNeutralWireActionCopy() {
        val pendingPlay = profile(CompanionAction.PLAY)
        val notReady = PetNotificationPolicy.evaluate(
            PetNotificationCursor(),
            PetNotificationSnapshot(address, true, pendingPlay, null, null, liveStateReady = false),
        )
        assertEquals(null, notReady.cursor.deviceBinding)
        assertTrue(notReady.events.isEmpty())

        val baseline = PetNotificationPolicy.evaluate(
            notReady.cursor,
            PetNotificationSnapshot(address, true, pendingPlay, null, null),
        )
        assertTrue(baseline.events.isEmpty())

        val changed = PetNotificationPolicy.evaluate(
            baseline.cursor,
            PetNotificationSnapshot(address, true, profile(CompanionAction.GIFT), null, null),
        )
        assertEquals(1, changed.events.size)
        assertEquals("Spend time together when you’re ready.", changed.events.single().text)
        assertTrue(changed.events.none { it.text.contains("gift", ignoreCase = true) })

        val duplicate = PetNotificationPolicy.evaluate(
            changed.cursor,
            PetNotificationSnapshot(address, true, profile(CompanionAction.GIFT), null, null),
        )
        assertTrue(duplicate.events.isEmpty())
    }

    @Test fun directReplyRequiresExactLiveDeviceCanonicalPeerAndUnshortenedText() {
        val owner = OwnerState(
            connection = ConnectionState(ConnectionMode.DIRECT_BLE, connected = true),
            activeDeviceAddress = address,
        )
        val ready = DirectReplyPolicy.evaluate(owner, address.lowercase(), peer, "hello")
        assertTrue(ready is DirectReplyEligibility.Ready)
        assertEquals("hello", (ready as DirectReplyEligibility.Ready).text)

        assertTrue(
            DirectReplyPolicy.evaluate(owner.copy(connection = ConnectionState()), address, peer, "hello")
                is DirectReplyEligibility.Rejected,
        )
        assertTrue(
            DirectReplyPolicy.evaluate(owner, "02:00:00:00:00:33", peer, "hello")
                is DirectReplyEligibility.Rejected,
        )
        assertTrue(
            DirectReplyPolicy.evaluate(owner, address, peer, "x".repeat(129))
                is DirectReplyEligibility.Rejected,
        )
    }

    private fun directSnapshot(
        messages: List<Message>,
        blocked: Set<String> = emptySet(),
    ) = DirectMessageNotificationSnapshot(
        deviceAddress = address,
        connected = true,
        journalSession = "boot",
        messages = messages,
        blockedPeerIds = blocked,
    )

    private fun directMessage(id: String, session: String = "boot") = Message(
        id = id,
        cursor = id,
        direction = "inbound",
        peerId = peer,
        text = "message $id",
        state = "received",
        occurredAt = id.toLongOrNull() ?: 0L,
        journalSession = session,
        senderName = "Shade",
        route = "direct",
    )

    private fun profile(action: CompanionAction) = CompanionProfile(
        ok = true,
        schema = PET_FEATURE_SCHEMA_VERSION,
        nickname = "Shade",
        personality = CompanionPersonality(PetPersonality.GENTLE, 70, 60, 50, 80),
        mood = CompanionMood.CONTENT,
        bond = CompanionBond(0, 0, 0, 0),
        favorite = null,
        routine = null,
        ritual = null,
        preferences = CompanionPreferences(null, null, null),
        checkIn = CompanionCheckIn(
            request = CompanionRequest(CompanionRequestState.PENDING, action),
            question = null,
            comfort = CompanionComfort(CompanionComfortKind.NONE, "", ""),
            callbackReady = false,
        ),
        goal = CompanionDailyGoal(CompanionGoalKind.CARE, CompanionAction.PET, 0, 2),
        development = CompanionDevelopment(0, 0, 0, 0, CompanionPersonalBests(0, 0, 0)),
        settings = CompanionSettings(
            CompanionQuickAction.PET,
            CompanionQuietHours(false, 0, 0),
        ),
        latestMemory = null,
    )
}
