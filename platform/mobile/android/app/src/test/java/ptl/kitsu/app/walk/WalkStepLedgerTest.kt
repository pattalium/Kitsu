package ptl.kitsu.app.walk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.PetFeaturePolicy

/** Deterministic tests for sensor-independent, physical-board-scoped crediting. */
class WalkStepLedgerTest {
    @Test
    fun newRouteUsesCurrentPhoneCounterAsItsAbsoluteBaseline() {
        val beforeRoute = WalkStepLedger().observeCounter(10_000L).next
        val bound = beforeRoute.bindRoute(DEVICE_A, routeId = 7L, firmwareStepsTotal = 120L).next

        assertEquals(DEVICE_A, bound.activeCheckpoint?.deviceAddress)
        assertEquals(10_000L, bound.activeCheckpoint?.lastCounter)
        assertEquals(120L, bound.activeCheckpoint?.stepsTotal)

        val walked = bound.observeCounter(10_015L).next
        assertEquals(135L, walked.activeCheckpoint?.stepsTotal)
        assertEquals(10_015L, walked.activeCheckpoint?.lastCounter)
    }

    @Test
    fun sameBoardAndRouteRebindNeverRegressesOrDoubleCredits() {
        var ledger = WalkStepLedger()
            .observeCounter(1_000L).next
            .bindRoute(DEVICE_A, 9L, 100L).next
            .observeCounter(1_010L).next
        assertEquals(110L, ledger.activeCheckpoint?.stepsTotal)

        val staleFirmwareRebind = ledger.bindRoute(DEVICE_A.lowercase(), 9L, 105L)
        assertFalse(staleFirmwareRebind.storageChanged)
        ledger = staleFirmwareRebind.next.observeCounter(1_020L).next
        assertEquals(120L, ledger.activeCheckpoint?.stepsTotal)

        ledger = ledger.bindRoute(DEVICE_A, 9L, 125L).next
        assertEquals(125L, ledger.activeCheckpoint?.stepsTotal)
        assertEquals(1_020L, ledger.activeCheckpoint?.lastCounter)
        ledger = ledger.observeCounter(1_025L).next
        assertEquals(130L, ledger.activeCheckpoint?.stepsTotal)
    }

    @Test
    fun processDeathRestoresStillActiveBoardAndCreditsOnlyDurableDelta() {
        val restoredState = WalkStepCheckpointSet(
            checkpoints = listOf(checkpoint(DEVICE_A, 17L, counter = 4_000L, total = 250L)),
            activeKey = WalkStepRouteKey(DEVICE_A, 17L),
        )
        var restored = WalkStepLedger(persisted = restoredState)

        // MainViewModel rebinding the same selected board must not erase its durable baseline.
        restored = restored.bindRoute(DEVICE_A, 17L, 240L).next
        val afterProcessDeath = restored.observeCounter(4_030L).next
        assertEquals(280L, afterProcessDeath.activeCheckpoint?.stepsTotal)

        val restoredAgain = WalkStepLedger(persisted = afterProcessDeath.persisted)
        val nextProcess = restoredAgain.observeCounter(4_040L).next
        assertEquals(290L, nextProcess.activeCheckpoint?.stepsTotal)
    }

    @Test
    fun sameRouteIdOnTwoBoardsNeverSharesCreditAndReturningBoardRebases() {
        var ledger = WalkStepLedger()
            .observeCounter(800L).next
            .bindRoute(DEVICE_A, 7L, 100L).next
            .observeCounter(825L).next
        assertEquals(125L, ledger.activeCheckpoint?.stepsTotal)

        ledger = ledger.bindRoute(DEVICE_B, 7L, 40L).next
        assertEquals(40L, ledger.activeCheckpoint?.stepsTotal)
        assertNull(ledger.activeCheckpoint?.lastCounter)
        // First callback after the switch is a fresh baseline, even if Android batched old steps.
        ledger = ledger.observeCounter(830L).next
        assertEquals(40L, ledger.activeCheckpoint?.stepsTotal)
        ledger = ledger.observeCounter(835L).next
        assertEquals(45L, ledger.activeCheckpoint?.stepsTotal)

        // Returning to A also waits for a fresh callback. B's time is never credited to A.
        ledger = ledger.bindRoute(DEVICE_A, 7L, 120L).next
        assertEquals(125L, ledger.activeCheckpoint?.stepsTotal)
        assertNull(ledger.activeCheckpoint?.lastCounter)
        ledger = ledger.observeCounter(840L).next
        assertEquals(125L, ledger.activeCheckpoint?.stepsTotal)
        ledger = ledger.observeCounter(846L).next
        assertEquals(131L, ledger.activeCheckpoint?.stepsTotal)

        val totals = ledger.persisted.checkpoints.associate { it.deviceAddress to it.stepsTotal }
        assertEquals(131L, totals[DEVICE_A])
        assertEquals(45L, totals[DEVICE_B])
    }

    @Test
    fun selectingIdleBoardStopsOldCreditAndIdleClearRemovesOnlyThatBoard() {
        var ledger = WalkStepLedger()
            .observeCounter(100L).next
            .bindRoute(DEVICE_A, 1L, 0L).next
            .observeCounter(110L).next
            .bindRoute(DEVICE_B, 2L, 20L).next
            .observeCounter(115L).next
            .observeCounter(120L).next
            .bindRoute(DEVICE_A, 1L, 10L).next
            .observeCounter(125L).next
            .observeCounter(130L).next
        assertEquals(15L, ledger.activeCheckpoint?.stepsTotal)

        val idleB = ledger.selectDevice(DEVICE_B)
        assertTrue(idleB.deviceRebased)
        assertNull(idleB.next.activeCheckpoint)
        ledger = idleB.next.clearDevice(DEVICE_B).next.observeCounter(140L).next

        val remaining = ledger.persisted.checkpoints.single()
        assertEquals(DEVICE_A, remaining.deviceAddress)
        assertEquals(15L, remaining.stepsTotal)
        assertNull(ledger.activeCheckpoint)
    }

    @Test
    fun exactClearCannotEraseOtherBoardWithSameRouteId() {
        var ledger = WalkStepLedger()
            .observeCounter(10L).next
            .bindRoute(DEVICE_A, 7L, 1L).next
            .bindRoute(DEVICE_B, 7L, 2L).next

        ledger = ledger.clearRoute(DEVICE_A, 7L).next
        assertEquals(listOf(DEVICE_B), ledger.persisted.checkpoints.map { it.deviceAddress })
        assertEquals(DEVICE_B, ledger.activeCheckpoint?.deviceAddress)

        val staleClear = ledger.clearRoute(DEVICE_A, 7L)
        assertFalse(staleClear.storageChanged)
        assertEquals(DEVICE_B, staleClear.next.activeCheckpoint?.deviceAddress)
    }

    @Test
    fun retainsOnlyThreeMostRecentlyBoundPhysicalBoards() {
        var ledger = WalkStepLedger().observeCounter(1_000L).next
        listOf(DEVICE_A, DEVICE_B, DEVICE_C, DEVICE_D).forEachIndexed { index, address ->
            ledger = ledger.bindRoute(address, (index + 1).toLong(), index.toLong()).next
        }

        assertEquals(
            listOf(DEVICE_B, DEVICE_C, DEVICE_D),
            ledger.persisted.checkpoints.map { it.deviceAddress },
        )
        assertEquals(DEVICE_D, ledger.activeCheckpoint?.deviceAddress)
    }

    @Test
    fun phoneCounterRollbackRebasesWithoutNegativeOrDuplicateCredit() {
        val restored = WalkStepLedger(
            persisted = WalkStepCheckpointSet(
                listOf(checkpoint(DEVICE_A, 21L, 50_000L, 600L)),
                WalkStepRouteKey(DEVICE_A, 21L),
            ),
        )
        val rebootSample = restored.observeCounter(20L)

        assertTrue(rebootSample.counterRebased)
        assertEquals(600L, rebootSample.next.activeCheckpoint?.stepsTotal)
        assertEquals(20L, rebootSample.next.activeCheckpoint?.lastCounter)

        val afterRebootWalk = rebootSample.next.observeCounter(27L)
        assertFalse(afterRebootWalk.counterRebased)
        assertEquals(607L, afterRebootWalk.next.activeCheckpoint?.stepsTotal)
    }

    @Test
    fun firmwareMaximumSaturatesButStillAdvancesThePhoneBaseline() {
        val ledger = WalkStepLedger(
            persisted = WalkStepCheckpointSet(
                listOf(
                    checkpoint(
                        DEVICE_A,
                        5L,
                        100L,
                        PetFeaturePolicy.MAX_WALK_STEPS - 1L,
                    ),
                ),
                WalkStepRouteKey(DEVICE_A, 5L),
            ),
        )
        val saturated = ledger.observeCounter(150L).next
        assertEquals(PetFeaturePolicy.MAX_WALK_STEPS, saturated.activeCheckpoint?.stepsTotal)
        assertEquals(150L, saturated.activeCheckpoint?.lastCounter)

        val later = saturated.observeCounter(200L).next
        assertEquals(PetFeaturePolicy.MAX_WALK_STEPS, later.activeCheckpoint?.stepsTotal)
        assertEquals(200L, later.activeCheckpoint?.lastCounter)
    }

    @Test
    fun checkpointSetCodecPreservesDeviceKeysOrderActiveKeyAndCrc() {
        val state = WalkStepCheckpointSet(
            checkpoints = listOf(
                checkpoint(DEVICE_A, 4_294_967_295L, 123_456L, 99_999L),
                checkpoint(DEVICE_B, 8L, null, 2L),
            ),
            activeKey = WalkStepRouteKey(DEVICE_B, 8L),
        )
        val encoded = WalkStepCheckpointCodec.encode(state)
        assertTrue(encoded.size <= WalkStepCheckpointCodec.ENCODED_MAX_BYTES)
        assertEquals(state, WalkStepCheckpointCodec.decode(encoded))

        val corrupted = encoded.copyOf().also { bytes ->
            bytes[17] = (bytes[17].toInt() xor 0x40).toByte()
        }
        assertNull(WalkStepCheckpointCodec.decode(corrupted))
        assertNull(WalkStepCheckpointCodec.decode(encoded + 0))
        assertNull(WalkStepCheckpointCodec.decode(ByteArray(encoded.size)))
    }

    @Test
    fun invalidDeviceRouteAndCounterBoundsFailBeforeStateCanChange() {
        assertThrows(IllegalArgumentException::class.java) {
            WalkStepLedger().bindRoute(" ", 1L, 0L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            WalkStepLedger().bindRoute(DEVICE_A, 0L, 0L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            WalkStepLedger().bindRoute(DEVICE_A, 1L, PetFeaturePolicy.MAX_WALK_STEPS + 1L)
        }
        assertThrows(IllegalArgumentException::class.java) {
            WalkStepLedger().observeCounter(-1L)
        }
    }

    @Test
    fun permissionPolicyStartsAtAndroidTenAndSnapshotMatchesExactDeviceAndRoute() {
        assertFalse(WalkStepPermissionPolicy.permissionRequired(28, permissionGranted = false))
        assertTrue(WalkStepPermissionPolicy.permissionRequired(29, permissionGranted = false))
        assertFalse(WalkStepPermissionPolicy.permissionRequired(29, permissionGranted = true))

        val snapshot = WalkStepSnapshot(
            availability = WalkStepAvailability.PERMISSION_REQUIRED,
            observing = false,
            deviceAddress = DEVICE_A,
            routeId = 7L,
            requiredPermission = WalkStepPermissionPolicy.ACTIVITY_RECOGNITION_PERMISSION,
        )
        assertTrue(snapshot.permissionRequired)
        assertFalse(snapshot.available)
        assertTrue(snapshot.matches(DEVICE_A.lowercase(), 7L))
        assertFalse(snapshot.matches(DEVICE_B, 7L))
        assertEquals("android.permission.ACTIVITY_RECOGNITION", snapshot.requiredPermission)
    }

    private fun checkpoint(
        deviceAddress: String,
        routeId: Long,
        counter: Long?,
        total: Long,
    ) = WalkStepCheckpoint(deviceAddress, routeId, counter, total)

    private companion object {
        const val DEVICE_A = "00:11:22:33:44:55"
        const val DEVICE_B = "AA:BB:CC:DD:EE:01"
        const val DEVICE_C = "AA:BB:CC:DD:EE:02"
        const val DEVICE_D = "AA:BB:CC:DD:EE:03"
    }
}
