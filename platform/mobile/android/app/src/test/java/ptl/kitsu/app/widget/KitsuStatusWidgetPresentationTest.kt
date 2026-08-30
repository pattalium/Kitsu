package ptl.kitsu.app.widget

import java.time.ZoneId
import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionCheckIn
import ptl.kitsu.app.model.CompanionComfort
import ptl.kitsu.app.model.CompanionComfortKind
import ptl.kitsu.app.model.CompanionRequest
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusCompletion
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusPrompt
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.model.PetPersonality
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkOutcome
import ptl.kitsu.app.model.WalkPhase
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather

class KitsuStatusWidgetPresentationTest {
    @Test fun rendersExactPetStatusAndDistinguishesLiveFromCachedFreshness() {
        val status = status()
        val live = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(status, connected = true, snapshotAtEpochSeconds = 9_970),
            nowEpochSeconds = 10_000,
        )
        val cached = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(status, connected = false, snapshotAtEpochSeconds = 9_280),
            nowEpochSeconds = 10_000,
        )

        assertEquals("Shade", live.petName)
        assertEquals("Mood · Content", live.moodText)
        assertEquals("Energy 73%", live.energyText)
        assertEquals(73, live.energyPercent)
        assertEquals("Battery 82%", live.batteryText)
        assertEquals("Connected · updated just now", live.freshnessText)
        assertEquals("Last synced 12m ago", cached.freshnessText)
        assertEquals(0x6C393E21L, live.portraitPackId)
    }

    @Test fun missingOrIncompleteStatusUsesHonestFallbacks() {
        val empty = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(null, connected = false, snapshotAtEpochSeconds = null),
            nowEpochSeconds = 10_000,
        )
        val voltageOnly = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(
                status().copy(packReady = false, batteryPercent = null, batteryMillivolts = 3_975),
                connected = false,
                snapshotAtEpochSeconds = 1,
            ),
            nowEpochSeconds = 10_000,
            zoneId = ZoneId.of("UTC"),
            locale = Locale.US,
        )

        assertEquals("Your Kitsu", empty.petName)
        assertEquals("Mood unknown", empty.moodText)
        assertEquals("Energy —", empty.energyText)
        assertEquals("Battery —", empty.batteryText)
        assertEquals("Open Kitsu to load status", empty.freshnessText)
        assertNull(empty.portraitPackId)
        assertEquals("Battery 3.98 V", voltageOnly.batteryText)
        assertNull(voltageOnly.portraitPackId)
    }

    @Test fun requestFocusAndWalkRowsAppearOnlyForActionableStates() {
        val actionable = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(
                status = status(),
                connected = true,
                snapshotAtEpochSeconds = 10_000,
                companionCheckIn = CompanionCheckIn(
                    request = CompanionRequest(CompanionRequestState.PENDING, CompanionAction.PLAY),
                    question = null,
                    comfort = CompanionComfort(CompanionComfortKind.NONE, "", ""),
                    callbackReady = false,
                ),
                focusSession = FocusSessionState(
                    ok = true,
                    schema = 1,
                    phase = FocusPhase.FOCUS,
                    completion = FocusCompletion.NONE,
                    sessionId = 7,
                    focusMinutes = 25,
                    breakMinutes = 5,
                    elapsedMs = 60_000,
                    remainingMs = 121_000,
                    sequence = 2,
                    prompt = FocusPrompt("Focus", "Stay with it", false),
                ),
                walkAdventure = activeWalk(),
            ),
            nowEpochSeconds = 10_000,
        )

        assertEquals("Request · Play", actionable.requestText)
        assertEquals("Focus · 3m left", actionable.focusText)
        assertEquals("Walk · 42% · 840 steps", actionable.walkText)

        val quiet = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(
                status = status(),
                connected = true,
                snapshotAtEpochSeconds = 10_000,
                companionCheckIn = actionableCheckIn().copy(
                    request = CompanionRequest(CompanionRequestState.COMPLETED, CompanionAction.PLAY),
                ),
                focusSession = actionableFocus().copy(phase = FocusPhase.IDLE),
                walkAdventure = activeWalk().copy(phase = WalkPhase.IDLE),
            ),
            nowEpochSeconds = 10_000,
        )
        assertNull(quiet.requestText)
        assertNull(quiet.focusText)
        assertNull(quiet.walkText)

        val neutralWireAction = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(
                status = status(),
                connected = true,
                snapshotAtEpochSeconds = 10_000,
                companionCheckIn = actionableCheckIn().copy(
                    request = CompanionRequest(CompanionRequestState.PENDING, CompanionAction.GIFT),
                ),
            ),
            nowEpochSeconds = 10_000,
        )
        assertEquals("Request · Spend time together", neutralWireAction.requestText)
    }

    @Test fun coldProjectionUsesEncryptedSnapshotTimeAndNeverClaimsAConnection() {
        val source = KitsuStatusWidgetSnapshotLoader.fromCache(
            CacheSnapshot(status = status(), writtenAt = 9_900, deviceAddress = "AA"),
        )
        val presentation = KitsuStatusWidgetPresentationPolicy.present(source, nowEpochSeconds = 10_000)

        assertEquals(false, source.connected)
        assertEquals(9_900L, source.snapshotAtEpochSeconds)
        assertEquals("Last synced 1m ago", presentation.freshnessText)
        assertTrue(presentation.contentDescription.contains("Shade"))
    }

    private fun status() = KitsuStatus(
        deviceId = "KT0001",
        companionName = "Shade",
        mood = "CONTENT",
        batteryPercent = 82,
        batteryMillivolts = 3_990,
        packReady = true,
        packId = 0x6C393E21L.toString(),
        needs = NeedLevels(energy = 73, curiosity = 61, affection = 88),
        updatedAt = 9_970,
    )

    private fun actionableCheckIn() = CompanionCheckIn(
        request = CompanionRequest(CompanionRequestState.PENDING, CompanionAction.PLAY),
        question = null,
        comfort = CompanionComfort(CompanionComfortKind.NONE, "", ""),
        callbackReady = false,
    )

    private fun actionableFocus() = FocusSessionState(
        ok = true,
        schema = 1,
        phase = FocusPhase.FOCUS,
        completion = FocusCompletion.NONE,
        sessionId = 7,
        focusMinutes = 25,
        breakMinutes = 5,
        elapsedMs = 60_000,
        remainingMs = 121_000,
        sequence = 2,
        prompt = FocusPrompt("Focus", "Stay with it", false),
    )

    private fun activeWalk() = WalkAdventureState(
        ok = true,
        schema = 1,
        phase = WalkPhase.ACTIVE,
        outcome = WalkOutcome.NONE,
        routeId = 5,
        steps = 840,
        targetSteps = 2_000,
        progressPercent = 42,
        distanceMeters = 610,
        terrain = WalkTerrain.FOREST,
        objective = WalkObjective.EXPLORE,
        risk = WalkRisk.BALANCED,
        weather = WalkWeather.CLEAR,
        personality = PetPersonality.CURIOUS,
        decisionCount = 0,
        branch = 0,
        privacy = WalkPrivacy.COARSE,
        currentZone = 1,
        homeZone = 1,
        knownZones = 2,
        totalDistanceMeters = 3_200,
        journalCount = 1,
        postcard = null,
    )
}
