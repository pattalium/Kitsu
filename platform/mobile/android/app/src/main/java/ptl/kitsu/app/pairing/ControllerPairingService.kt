package ptl.kitsu.app.pairing

import ptl.kitsu.app.security.BondedCompanion

enum class ControllerPairingStage {
    SCANNING,
    OS_SECURE_PAIRING,
    CONNECTING_GATT,
    WAITING_FOR_PRG,
    SAVING_CONTROLLER,
    COMPLETE,
    CANCELLED,
}

enum class BluetoothPairingRepairStage {
    CHECKING_SAVED_CONTROLLER,
    SCANNING,
    OS_SECURE_PAIRING,
    BOND_COMPLETE,
    CONNECTING_GATT,
    VERIFYING_CONTROLLER,
    COMPLETE,
    CANCELLED,
}

data class ControllerPairingProgress(
    val stage: ControllerPairingStage,
    val detail: String,
)

data class BluetoothPairingRepairProgress(
    val stage: BluetoothPairingRepairStage,
    val detail: String,
)

/** Native first-run controller issuance over authenticated Bluetooth. */
interface ControllerPairingService {
    suspend fun pairController(
        label: String,
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion

    suspend fun finishPendingPairing(
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion

    /**
     * Recreates only Android's BLE/SMP bond for an already-authorized controller.
     *
     * Implementations must return the exact saved controller capability and must
     * never run controller issuance or replace the saved controller ID/root.
     */
    suspend fun repairBluetoothPairing(
        deviceAddress: String,
        onProgress: (BluetoothPairingRepairProgress) -> Unit,
    ): BondedCompanion

    fun cancelPairing()
}
