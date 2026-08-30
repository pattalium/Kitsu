package ptl.kitsu.app.pairing

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.security.BondedCompanion

class BluetoothPairingRepairPolicyTest {
    @Test fun onlyIndependentBondEvidenceOffersDedicatedRepair() {
        assertTrue(BluetoothPairingRepairPolicy.shouldOfferRepair(
            BluetoothPairingRepairPolicy.REPAIR_REQUIRED,
        ))
        assertTrue(BluetoothPairingRepairPolicy.shouldOfferRepair(
            BluetoothPairingRepairPolicy.BOND_MISSING,
        ))
        assertFalse(BluetoothPairingRepairPolicy.shouldOfferRepair("gatt_peer_terminated"))
        assertFalse(BluetoothPairingRepairPolicy.shouldOfferRepair("gatt_peer_terminated_before_auth"))
        assertFalse(BluetoothPairingRepairPolicy.shouldOfferRepair("gatt_status_19"))
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

    @Test fun postBondGattRetryIsLimitedToOneLocalHostTermination() {
        assertTrue(BluetoothPairingRepairPolicy.shouldRetryPostBondGatt(
            "gatt_local_host_terminated",
            retriesUsed = 0,
        ))
        assertFalse(BluetoothPairingRepairPolicy.shouldRetryPostBondGatt(
            "gatt_local_host_terminated",
            retriesUsed = 1,
        ))
        assertFalse(BluetoothPairingRepairPolicy.shouldRetryPostBondGatt(
            "gatt_timeout",
            retriesUsed = 0,
        ))
        assertFalse(BluetoothPairingRepairPolicy.shouldRetryPostBondGatt(null, retriesUsed = 0))
    }

    @Test fun localForgetRequiresExactCredentialAddressAndAuthoritativeProof() {
        val saved = companion("root-a")
        val replaced = companion("root-b")
        assertTrue(ControllerForgetPolicy.mayCompleteLocally(
            provenMissing = saved,
            current = saved,
            requestedAddress = saved.deviceAddress.lowercase(),
        ))
        assertFalse(ControllerForgetPolicy.mayCompleteLocally(
            provenMissing = saved,
            current = replaced,
            requestedAddress = saved.deviceAddress,
        ))
        assertFalse(ControllerForgetPolicy.mayCompleteLocally(
            provenMissing = saved,
            current = saved,
            requestedAddress = "AA:BB:CC:DD:EE:FF",
        ))
        assertFalse(ControllerForgetPolicy.mayCompleteLocally(
            provenMissing = null,
            current = saved,
            requestedAddress = saved.deviceAddress,
        ))
    }

    private fun companion(root: String) = BondedCompanion(
        deviceAddress = "00:11:22:33:44:55",
        displayName = "Kitsu",
        controllerIdB64 = "controller",
        controllerRootB64 = root,
    )
}
