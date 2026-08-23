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

data class ControllerPairingProgress(
    val stage: ControllerPairingStage,
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

    fun cancelPairing()
}
