package ptl.kitsu.app.pairing

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BluetoothPairingRepairPolicyTest {
    @Test fun status19AndMissingBondOfferDedicatedRepair() {
        assertTrue(BluetoothPairingRepairPolicy.shouldOfferRepair(
            BluetoothPairingRepairPolicy.REPAIR_REQUIRED,
        ))
        assertTrue(BluetoothPairingRepairPolicy.shouldOfferRepair(
            BluetoothPairingRepairPolicy.BOND_MISSING,
        ))
        assertFalse(BluetoothPairingRepairPolicy.shouldOfferRepair("gatt_status_133"))
    }

    @Test fun staleAndroidBondRequiresExplicitSystemForget() {
        assertTrue(BluetoothPairingRepairPolicy.requiresAndroidForget(
            BluetoothPairingRepairPolicy.ANDROID_FORGET_REQUIRED,
        ))
        assertFalse(BluetoothPairingRepairPolicy.requiresAndroidForget(
            BluetoothPairingRepairPolicy.BOND_MISSING,
        ))
    }

    @Test fun repairRemainsAvailableAtFullControllerCapacity() {
        assertTrue(BluetoothPairingRepairPolicy.availableForSavedController(true, 4))
        assertFalse(BluetoothPairingRepairPolicy.availableForSavedController(false, 0))
    }

    @Test fun explicitForgetCanFinishWhenFirmwareRejectsTheStaleLocalRoot() {
        assertTrue(ControllerForgetPolicy.controllerAlreadyAbsent(
            ControllerForgetPolicy.AUTHORIZATION_REJECTED,
        ))
        assertFalse(ControllerForgetPolicy.controllerAlreadyAbsent("controller_auth_failed"))
        assertFalse(ControllerForgetPolicy.controllerAlreadyAbsent("gatt_status_133"))
        assertFalse(ControllerForgetPolicy.controllerAlreadyAbsent(null))
    }
}
