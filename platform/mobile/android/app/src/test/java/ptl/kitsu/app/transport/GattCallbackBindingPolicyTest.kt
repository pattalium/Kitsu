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
        assertEquals("gatt_status_133", GattStatusPolicy.connectionFailure(133))
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
}
