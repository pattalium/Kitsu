package app.kitsu.mobile.pairing

import app.kitsu.mobile.security.BondedCompanion

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

/** Native first-run controller issuance. It never falls back to the backend. */
interface ControllerPairingService {
    suspend fun pairController(
        label: String,
        onProgress: (ControllerPairingProgress) -> Unit,
    ): BondedCompanion

    fun cancelPairing()
}
