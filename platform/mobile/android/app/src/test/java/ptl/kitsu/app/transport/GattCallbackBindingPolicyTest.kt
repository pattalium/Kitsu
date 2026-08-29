package ptl.kitsu.app.transport

import java.util.UUID
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GattCallbackBindingPolicyTest {
    @Test fun lateNotificationFromReplacedGattCannotEnterTheNewSessionDecoder() {
        val notify = UUID.randomUUID()
        val staleGatt = Any()
        val activeGatt = Any()

        assertFalse(GattCallbackBindingPolicy.accepts(activeGatt, staleGatt, notify, notify))
        assertFalse(GattCallbackBindingPolicy.accepts(activeGatt, activeGatt, notify, UUID.randomUUID()))
        assertTrue(GattCallbackBindingPolicy.accepts(activeGatt, activeGatt, notify, notify))
    }

    @Test fun everyLifecycleCallbackCanRejectAReplacedGattByIdentity() {
        val staleGatt = Any()
        val activeGatt = Any()

        assertFalse(GattCallbackBindingPolicy.accepts(activeGatt, staleGatt))
        assertFalse(GattCallbackBindingPolicy.accepts(null, staleGatt))
        assertTrue(GattCallbackBindingPolicy.accepts(activeGatt, activeGatt))
    }

    @Test fun peerSecurityTerminationBecomesAnActionableRepairCode() {
        assertEquals(
            "bluetooth_pairing_repair_required",
            GattStatusPolicy.connectionFailure(19),
        )
        assertEquals(
            "gatt_local_host_terminated",
            GattStatusPolicy.connectionFailure(22),
        )
        assertEquals("gatt_status_133", GattStatusPolicy.connectionFailure(133))
    }

    @Test fun freshBondMayRecoverFromLocalHostTerminationExactlyOnce() {
        assertTrue(FreshBondGattRetryPolicy.shouldRetry(
            freshBond = true,
            retriesUsed = 0,
            failureCode = "gatt_local_host_terminated",
        ))
        assertFalse(FreshBondGattRetryPolicy.shouldRetry(
            freshBond = true,
            retriesUsed = 1,
            failureCode = "gatt_local_host_terminated",
        ))
        assertFalse(FreshBondGattRetryPolicy.shouldRetry(
            freshBond = false,
            retriesUsed = 0,
            failureCode = "gatt_local_host_terminated",
        ))
        assertFalse(FreshBondGattRetryPolicy.shouldRetry(
            freshBond = true,
            retriesUsed = 0,
            failureCode = "gatt_status_133",
        ))
        assertTrue(FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
            freshBond = true,
            retriesUsed = 0,
            failureCode = "gatt_local_host_terminated",
            pairingPendingSeen = false,
            candidateStored = false,
        ))
        assertFalse(FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
            freshBond = true,
            retriesUsed = 0,
            failureCode = "gatt_local_host_terminated",
            pairingPendingSeen = false,
            candidateStored = true,
        ))
        assertFalse(FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
            freshBond = true,
            retriesUsed = 0,
            failureCode = "gatt_local_host_terminated",
            pairingPendingSeen = true,
            candidateStored = false,
        ))
    }

    @Test fun status22DuringPairRequestWriteRetainsItsBoundedRecoveryReason() {
        val completionStatus = GattWriteFailurePolicy.completionStatus(22)
        val failure = GattWriteFailurePolicy.pairingFailure(
            GattStatusPolicy.connectionFailure(completionStatus),
        )

        assertEquals(22, completionStatus)
        assertEquals("gatt_local_host_terminated", failure)
        assertTrue(FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
            freshBond = true,
            retriesUsed = 0,
            failureCode = failure,
            pairingPendingSeen = false,
            candidateStored = false,
        ))
    }

    @Test fun ordinaryWriteTimeoutRemainsGenericAndNeverEntersStatus22Recovery() {
        val failure = GattWriteFailurePolicy.pairingFailure(null)

        assertEquals("gatt_write_failed", failure)
        assertFalse(FreshBondGattRetryPolicy.shouldRetryBeforeGrant(
            freshBond = true,
            retriesUsed = 0,
            failureCode = failure,
            pairingPendingSeen = false,
            candidateStored = false,
        ))
        assertTrue(GattWriteFailurePolicy.completionStatus(0) != 0)
    }

    @Test fun securedCccdAuthenticationFailuresBecomeAnActionableRepairCode() {
        assertEquals(
            "bluetooth_pairing_repair_required",
            GattStatusPolicy.notificationSubscriptionFailure(5),
        )
        assertEquals(
            "bluetooth_pairing_repair_required",
            GattStatusPolicy.notificationSubscriptionFailure(15),
        )
        assertEquals(
            "notify_descriptor_write_failed",
            GattStatusPolicy.notificationSubscriptionFailure(257),
        )
    }

    @Test fun explicitControllerRejectionIsNeverCollapsedIntoAmbiguousAuthLoss() {
        assertEquals(
            "controller_authorization_rejected",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "controller_rejected",
                distinguishPendingRejection = false,
            ),
        )
        assertEquals(
            "pending_controller_rejected",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "controller_rejected",
                distinguishPendingRejection = true,
            ),
        )
        assertEquals(
            "controller_authorization_rejected",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "controller_proof_rejected",
                distinguishPendingRejection = false,
            ),
        )
        assertEquals(
            "pending_controller_rejected",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "controller_proof_rejected",
                distinguishPendingRejection = true,
            ),
        )
        assertEquals(
            "controller_auth_failed",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "handshake_timeout",
                distinguishPendingRejection = false,
            ),
        )
        assertEquals(
            "controller_auth_failed",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = "auth_failed",
                distinguishPendingRejection = false,
            ),
        )
        assertEquals(
            "controller_auth_failed",
            ControllerHandshakeFailurePolicy.code(
                handshakeCode = null,
                distinguishPendingRejection = false,
            ),
        )
    }
}
