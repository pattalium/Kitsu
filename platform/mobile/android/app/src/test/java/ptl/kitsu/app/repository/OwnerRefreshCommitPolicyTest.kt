package ptl.kitsu.app.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.ExpeditionFunState
import ptl.kitsu.app.model.ExpeditionStatus
import ptl.kitsu.app.model.FUN_STATE_SCHEMA
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.PartyFunState
import ptl.kitsu.app.model.PartyPhase
import ptl.kitsu.app.model.PartyReward
import ptl.kitsu.app.model.PartyRewardTier
import ptl.kitsu.app.model.PartyRole
import ptl.kitsu.app.model.PartySignalChoice
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
import ptl.kitsu.app.model.StoryFunState
import ptl.kitsu.app.model.StoryStatus

class OwnerRefreshCommitPolicyTest {
    @Test fun delayedBroadRefreshPreservesAnInterveningMessageCommitAndItsFreshness() {
        val delivered = message("delivered")
        val current = OwnerState(
            messages = listOf(delivered),
            messagesErrorCode = null,
            errorCode = "history_timeout",
        )
        val broadReadStartedEarlier = OwnerNonMessageSnapshot(
            connection = ConnectionState(),
            status = KitsuStatus(deviceId = "KT0001", companionName = "Kitsu", updatedAt = 2),
            history = listOf(HistoryEntry("2", "2", "mesh", "heard", 2)),
            peers = emptyList(),
            channels = emptyList(),
            encounterCatalog = PUBLIC_ENCOUNTER_CATALOG,
            encounterCatalogSupported = true,
            nearbyInteractionKinds = setOf(
                NeighborInteractionKind.PET,
                NeighborInteractionKind.GREET,
                NeighborInteractionKind.PLAY,
                NeighborInteractionKind.GIFT,
            ),
        )

        val committed = OwnerRefreshCommitPolicy.apply(current, broadReadStartedEarlier)

        assertEquals(listOf(delivered), committed.messages)
        assertNull(committed.messagesErrorCode)
        assertEquals("KT0001", committed.status?.deviceId)
        assertEquals(PUBLIC_ENCOUNTER_CATALOG, committed.encounterCatalog)
        assertEquals(true, committed.encounterCatalogSupported)
        assertEquals(broadReadStartedEarlier.nearbyInteractionKinds, committed.nearbyInteractionKinds)
        assertNull(committed.errorCode)
    }

    @Test fun unrelatedEndpointFailureCannotDiscardAValidMessageSnapshot() {
        val received = message("received")
        val current = OwnerState(messages = listOf(received), messagesErrorCode = null, loading = true)

        val failed = OwnerRefreshCommitPolicy.applyFailure(
            current = current,
            connection = ConnectionState(),
            errorCode = "malformed_history",
        )

        assertEquals(listOf(received), failed.messages)
        assertNull(failed.messagesErrorCode)
        assertEquals("malformed_history", failed.errorCode)
    }

    @Test fun broadRefreshCommitsTheAuthenticatedFunSnapshotWithoutClearingMutationState() {
        val previous = idleFunState(partyBond = 2)
        val refreshed = idleFunState(partyBond = 9)
        val current = OwnerState(
            funState = previous,
            funSupported = true,
            funErrorCode = "fun_state_refresh_failed",
            funMutationInFlight = true,
        )
        val snapshot = OwnerNonMessageSnapshot(
            connection = ConnectionState(),
            status = KitsuStatus(deviceId = "KT0001", companionName = "Kitsu", updatedAt = 2),
            history = emptyList(),
            peers = emptyList(),
            channels = emptyList(),
            funState = refreshed,
            funSupported = true,
            funErrorCode = null,
        )

        val committed = OwnerRefreshCommitPolicy.apply(current, snapshot)

        assertSame(refreshed, committed.funState)
        assertTrue(committed.funSupported)
        assertNull(committed.funErrorCode)
        assertTrue(committed.funMutationInFlight)
    }

    @Test fun unsupportedFirmwareSnapshotClearsPriorFunData() {
        val current = OwnerState(
            funState = idleFunState(partyBond = 4),
            funSupported = true,
            funErrorCode = "old_error",
        )
        val snapshot = OwnerNonMessageSnapshot(
            connection = ConnectionState(),
            status = KitsuStatus(deviceId = "KT0001", companionName = "Kitsu", updatedAt = 2),
            history = emptyList(),
            peers = emptyList(),
            channels = emptyList(),
        )

        val committed = OwnerRefreshCommitPolicy.apply(current, snapshot)

        assertNull(committed.funState)
        assertFalse(committed.funSupported)
        assertNull(committed.funErrorCode)
    }

    private fun message(state: String) = Message(
        id = "4",
        cursor = "4",
        direction = "outbound",
        text = "hello",
        state = state,
        occurredAt = 1,
    )

    private fun idleFunState(partyBond: Long) = FunState(
        schema = FUN_STATE_SCHEMA,
        expedition = ExpeditionFunState(
            status = ExpeditionStatus.IDLE,
            duration = null,
            expeditionId = null,
            totalSeconds = 0,
            remainingSeconds = 0,
            progressPercent = 0,
            report = null,
        ),
        story = StoryFunState(
            status = StoryStatus.IDLE,
            beat = null,
            resolution = null,
        ),
        party = PartyFunState(
            role = PartyRole.NONE,
            phase = PartyPhase.IDLE,
            hostDeviceId = null,
            sessionNonce = null,
            discoveredHosts = emptyList(),
            participantCount = 0,
            participants = emptyList(),
            round = 0,
            localChoice = PartySignalChoice.NONE,
            reward = PartyReward(
                tier = PartyRewardTier.NONE,
                score = 0,
                maximumScore = 0,
                bondAwarded = 0,
                partyBond = partyBond,
                eligibleUniquePeers = 0,
                currentStreakDays = 0,
                longestStreakDays = 0,
            ),
        ),
    )
}
