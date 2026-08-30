package ptl.kitsu.app.ui

import ptl.kitsu.app.pairing.ControllerPairingFlow
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.security.ControllerRole

internal data class ControllerPairingCopy(
    val buttonLabel: String,
    val deviceInstruction: String,
    val accessSummary: String,
    val progressLabel: String,
)

internal object ControllerPairingPresentation {
    fun copy(flow: ControllerPairingFlow): ControllerPairingCopy = when (flow) {
        ControllerPairingFlow.OWNER -> ControllerPairingCopy(
            buttonLabel = "Pair as owner",
            deviceInstruction =
                "On Kitsu, open Connect and choose PAIR PHONE. Then start pairing here.",
            accessSummary = "Owner access includes every Kitsu and Mesh feature on this phone.",
            progressLabel = "Pairing owner phone",
        )
        ControllerPairingFlow.CARETAKER -> ControllerPairingCopy(
            buttonLabel = "Pair as caretaker",
            deviceInstruction =
                "On Kitsu, open Connect and choose PAIR CARETAKER. Continue only while Kitsu says CARETAKER.",
            accessSummary =
                "Caretakers can check on Kitsu and use care, Focus, and Walk. Messages, Mesh, device settings, and firmware updates stay hidden.",
            progressLabel = "Pairing caretaker phone",
        )
    }

    fun savedRoleLabel(role: ControllerRole): String = when (role) {
        ControllerRole.OWNER -> "Owner"
        ControllerRole.CARETAKER -> "Caretaker"
    }

    fun canRemoveRevokedCaretakerAuthorization(
        role: ControllerRole,
        deviceAddress: String,
        activeDeviceAddress: String?,
        errorCode: String?,
    ): Boolean = role == ControllerRole.CARETAKER &&
        deviceAddress.equals(activeDeviceAddress, ignoreCase = true) &&
        errorCode == BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING
}
