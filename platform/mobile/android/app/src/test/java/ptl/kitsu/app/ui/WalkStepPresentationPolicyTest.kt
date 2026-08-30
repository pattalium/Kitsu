package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.PetPersonality
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkOutcome
import ptl.kitsu.app.model.WalkPhase
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather
import ptl.kitsu.app.walk.WalkStepAvailability
import ptl.kitsu.app.walk.WalkStepSnapshot

class WalkStepPresentationPolicyTest {
    @Test
    fun missingPermissionRoutesPrimaryActionBeforeStartingTrackedWalk() {
        val snapshot = snapshot(WalkStepAvailability.PERMISSION_REQUIRED)

        assertTrue(WalkStepPresentationPolicy.requestsPermissionInsteadOfStarting(snapshot))
        assertTrue(WalkStepPresentationPolicy.requestsStepSetupInsteadOfStarting(snapshot))
        assertEquals("Allow steps to start", WalkStepPresentationPolicy.startLabel(snapshot))
        assertEquals(
            "Allow Physical activity to count phone steps during a Kitsu walk.",
            WalkStepPresentationPolicy.status(snapshot, walk(WalkPhase.IDLE), DEVICE_A),
        )
    }

    @Test
    fun failedOrPausedSensorRoutesPrimaryActionToRetry() {
        val failed = snapshot(WalkStepAvailability.REGISTRATION_FAILED)
        assertTrue(WalkStepPresentationPolicy.requestsStepSetupInsteadOfStarting(failed))
        assertEquals("Retry phone steps", WalkStepPresentationPolicy.startLabel(failed))

        val paused = snapshot(WalkStepAvailability.AVAILABLE, observing = false)
        assertTrue(WalkStepPresentationPolicy.requestsStepSetupInsteadOfStarting(paused))
        assertEquals("Enable phone steps", WalkStepPresentationPolicy.startLabel(paused))
    }

    @Test
    fun sensorCalibrationAndIdleScopeAreExplicit() {
        val calibrating = snapshot(
            availability = WalkStepAvailability.AVAILABLE,
            observing = true,
            sensorBaselineReady = false,
        )
        assertEquals(
            "Calibrating phone steps. Take a few steps with this phone before starting so the walk begins from a clean baseline.",
            WalkStepPresentationPolicy.status(calibrating, walk(WalkPhase.IDLE), DEVICE_A),
        )

        val calibrated = calibrating.copy(sensorBaselineReady = true)
        assertFalse(WalkStepPresentationPolicy.requestsPermissionInsteadOfStarting(calibrated))
        assertEquals("Start tracked walk", WalkStepPresentationPolicy.startLabel(calibrated))
        assertEquals(
            "Phone steps are calibrated. Automatic counting starts only after you start a Kitsu walk.",
            WalkStepPresentationPolicy.status(calibrated, walk(WalkPhase.IDLE), DEVICE_A),
        )
    }

    @Test
    fun activeWalkShowsExactPhoneAndKitsuTotalsOnlyForSelectedDevice() {
        val active = walk(WalkPhase.ACTIVE, steps = 120L)
        val snapshot = snapshot(
            availability = WalkStepAvailability.AVAILABLE,
            observing = true,
            sensorBaselineReady = true,
            deviceAddress = DEVICE_A,
            routeId = active.routeId,
            stepsTotal = 145L,
        )

        assertEquals(
            "Phone walk total: 145 steps · Kitsu total: 120 steps",
            WalkStepPresentationPolicy.status(snapshot, active, DEVICE_A),
        )
        assertEquals(
            "Linking phone steps to this Kitsu walk…",
            WalkStepPresentationPolicy.status(snapshot, active, DEVICE_B),
        )
    }

    private fun snapshot(
        availability: WalkStepAvailability,
        observing: Boolean = false,
        sensorBaselineReady: Boolean = false,
        deviceAddress: String? = null,
        routeId: Long? = null,
        stepsTotal: Long = 0L,
    ) = WalkStepSnapshot(
        availability = availability,
        observing = observing,
        sensorBaselineReady = sensorBaselineReady,
        deviceAddress = deviceAddress,
        routeId = routeId,
        stepsTotal = stepsTotal,
    )

    private fun walk(phase: WalkPhase, steps: Long = 0L) = WalkAdventureState(
        ok = true,
        schema = 1,
        phase = phase,
        outcome = WalkOutcome.NONE,
        routeId = if (phase == WalkPhase.IDLE) 0L else 7L,
        steps = steps,
        targetSteps = 2_000L,
        progressPercent = ((steps * 100L) / 2_000L).toInt(),
        distanceMeters = 0L,
        terrain = WalkTerrain.MEADOW,
        objective = WalkObjective.EXPLORE,
        risk = WalkRisk.BALANCED,
        weather = WalkWeather.UNKNOWN,
        personality = PetPersonality.CURIOUS,
        decisionCount = 0,
        branch = 0,
        privacy = WalkPrivacy.COARSE,
        currentZone = 1,
        homeZone = 1,
        knownZones = 1,
        totalDistanceMeters = 0L,
        journalCount = 0,
        postcard = null,
    )

    private companion object {
        const val DEVICE_A = "00:11:22:33:44:55"
        const val DEVICE_B = "AA:BB:CC:DD:EE:FF"
    }
}
