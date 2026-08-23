package ptl.kitsu.app.transport

import java.util.UUID
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
}
