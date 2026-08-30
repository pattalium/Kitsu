package ptl.kitsu.app.walk

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
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

class WalkStepSynchronizationTest {
    @Test
    fun automaticSyncIsExactDeviceScopedAndBecomesEligibleOnOtaUnlockWithoutNewSample() {
        val snapshot = snapshot(DEVICE_A, routeId = 7L, steps = 140L)
        val walk = walk(routeId = 7L, steps = 100L)
        val unlockRetry = WalkFirmwareUnlockRetry(initiallyLocked = false)

        assertNull(
            WalkStepSyncPolicy.automaticRequest(
                snapshot,
                DEVICE_A,
                walk,
                connected = true,
                firmwareControlsLocked = true,
            ),
        )
        assertFalse(unlockRetry.observe(locked = false))
        assertFalse(unlockRetry.observe(locked = true))
        assertTrue(unlockRetry.observe(locked = false))
        assertFalse(unlockRetry.observe(locked = false))
        // The exact same sensor snapshot is immediately eligible when firmware unlocks.
        assertEquals(
            WalkStepSyncRequest(DEVICE_A, 7L, 140L),
            WalkStepSyncPolicy.automaticRequest(
                snapshot,
                DEVICE_A,
                walk,
                connected = true,
                firmwareControlsLocked = false,
            ),
        )
        assertNull(
            WalkStepSyncPolicy.automaticRequest(
                snapshot,
                DEVICE_B,
                walk,
                connected = true,
                firmwareControlsLocked = false,
            ),
        )
    }

    @Test
    fun firmwareOperationWaitsForInFlightWalkSyncToDrain() = runTest {
        val gate = WalkStepOperationGate()
        val syncEntered = CompletableDeferred<Unit>()
        val releaseSync = CompletableDeferred<Unit>()
        val firmwareEntered = CompletableDeferred<Unit>()
        val events = mutableListOf<String>()

        val syncJob = launch {
            gate.withWalkOperation {
                events += "sync-start"
                syncEntered.complete(Unit)
                releaseSync.await()
                events += "sync-end"
            }
        }
        syncEntered.await()
        val firmwareJob = launch {
            gate.withFirmwareOperation {
                events += "firmware-start"
                firmwareEntered.complete(Unit)
            }
        }
        yield()
        assertFalse(firmwareEntered.isCompleted)

        releaseSync.complete(Unit)
        joinAll(syncJob, firmwareJob)
        assertEquals(listOf("sync-start", "sync-end", "firmware-start"), events)
    }

    @Test
    fun finishAndReturnSequenceSyncsLatestMatchingPhoneTotalBeforeTerminalMutation() = runTest {
        val gate = WalkStepOperationGate()
        val route = walk(routeId = 9L, steps = 100L)
        val events = mutableListOf<String>()

        val result = gate.withTerminalOperation(
            deviceAddress = DEVICE_A,
            walk = route,
            snapshot = { snapshot(DEVICE_A, 9L, 155L) },
            validateBinding = { events += "validate" },
            sync = { total ->
                events += "sync:$total"
                route.copy(steps = total)
            },
            terminal = {
                events += "terminal"
                "done"
            },
        )

        assertEquals("done", result)
        assertEquals(listOf("validate", "sync:155", "validate", "terminal"), events)
    }

    @Test
    fun terminalSequenceNeverSyncsAnotherBoardsSameRouteTotal() = runTest {
        val gate = WalkStepOperationGate()
        val route = walk(routeId = 9L, steps = 100L)
        var synced = false

        gate.withTerminalOperation(
            deviceAddress = DEVICE_A,
            walk = route,
            snapshot = { snapshot(DEVICE_B, 9L, 999L) },
            validateBinding = {},
            sync = {
                synced = true
                route
            },
            terminal = { Unit },
        )

        assertFalse(synced)
    }

    @Test
    fun terminalPolicyReadsSnapshotAtExecutionTime() = runTest {
        val gate = WalkStepOperationGate()
        val route = walk(routeId = 9L, steps = 100L)
        var liveSnapshot = snapshot(DEVICE_A, 9L, 110L)
        liveSnapshot = liveSnapshot.copy(stepsTotal = 175L)
        var syncedTotal: Long? = null

        gate.withTerminalOperation(
            deviceAddress = DEVICE_A,
            walk = route,
            snapshot = { liveSnapshot },
            validateBinding = {},
            sync = { total ->
                syncedTotal = total
                route.copy(steps = total)
            },
            terminal = { Unit },
        )

        assertEquals(175L, syncedTotal)
        assertTrue(liveSnapshot.matches(DEVICE_A, 9L))
    }

    private fun snapshot(device: String, routeId: Long, steps: Long) = WalkStepSnapshot(
        availability = WalkStepAvailability.AVAILABLE,
        observing = true,
        deviceAddress = device,
        routeId = routeId,
        stepsTotal = steps,
    )

    private fun walk(routeId: Long, steps: Long) = WalkAdventureState(
        ok = true,
        schema = 1,
        phase = WalkPhase.ACTIVE,
        outcome = WalkOutcome.NONE,
        routeId = routeId,
        steps = steps,
        targetSteps = 2_000,
        progressPercent = 5,
        distanceMeters = 80,
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
        knownZones = 1,
        totalDistanceMeters = 80,
        journalCount = 0,
        postcard = null,
    )

    private companion object {
        const val DEVICE_A = "00:11:22:33:44:55"
        const val DEVICE_B = "AA:BB:CC:DD:EE:FF"
    }
}
