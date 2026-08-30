package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.pairing.ControllerPairingFlow
import ptl.kitsu.app.security.ControllerRole

class ControllerPairingPresentationTest {
    @Test fun caretakerCopyRequiresThePhysicalCaretakerChoice() {
        val copy = ControllerPairingPresentation.copy(ControllerPairingFlow.CARETAKER)

        assertEquals("Pair as caretaker", copy.buttonLabel)
        assertTrue(copy.deviceInstruction.contains("PAIR CARETAKER"))
        assertTrue(copy.deviceInstruction.contains("Kitsu says CARETAKER"))
        assertTrue(copy.accessSummary.contains("Messages"))
        assertTrue(copy.accessSummary.contains("Mesh"))
        assertTrue(copy.accessSummary.contains("firmware updates"))
        assertFalse(copy.accessSummary.contains("inventory", ignoreCase = true))
    }

    @Test fun ownerAndCaretakerFlowsCannotBeConfused() {
        val owner = ControllerPairingPresentation.copy(ControllerPairingFlow.OWNER)
        val caretaker = ControllerPairingPresentation.copy(ControllerPairingFlow.CARETAKER)

        assertTrue(owner.deviceInstruction.contains("PAIR PHONE"))
        assertFalse(owner.deviceInstruction.contains("PAIR CARETAKER"))
        assertFalse(caretaker.deviceInstruction.contains("PAIR PHONE"))
        assertEquals("Owner", ControllerPairingPresentation.savedRoleLabel(ControllerRole.OWNER))
        assertEquals(
            "Caretaker",
            ControllerPairingPresentation.savedRoleLabel(ControllerRole.CARETAKER),
        )
    }

    @Test fun revokedCaretakerCanOnlyRemoveTheExactActiveRecoveryCredential() {
        val address = "00:11:22:33:44:55"

        assertTrue(
            ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                role = ControllerRole.CARETAKER,
                deviceAddress = address,
                activeDeviceAddress = address.lowercase(),
                errorCode = BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
            ),
        )
        assertFalse(
            ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                role = ControllerRole.OWNER,
                deviceAddress = address,
                activeDeviceAddress = address,
                errorCode = BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
            ),
        )
        assertFalse(
            ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                role = ControllerRole.CARETAKER,
                deviceAddress = address,
                activeDeviceAddress = "AA:BB:CC:DD:EE:FF",
                errorCode = BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
            ),
        )
        assertFalse(
            ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                role = ControllerRole.CARETAKER,
                deviceAddress = address,
                activeDeviceAddress = address,
                errorCode = "gatt_status_133",
            ),
        )
    }
}
