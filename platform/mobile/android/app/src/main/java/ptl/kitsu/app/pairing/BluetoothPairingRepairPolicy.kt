package ptl.kitsu.app.pairing

/** Pure policy shared by transport diagnostics and the repair UI. */
internal object BluetoothPairingRepairPolicy {
    const val REPAIR_REQUIRED = "bluetooth_pairing_repair_required"
    const val BOND_MISSING = "bond_missing_repair_required"
    const val ANDROID_FORGET_REQUIRED = "android_bluetooth_forget_required"
    const val PERMISSION_REQUIRED = "repair_bluetooth_permission_required"
    const val SAVED_CONTROLLER_MISSING = "saved_controller_authorization_missing"

    private val retryableCodes = setOf(
        REPAIR_REQUIRED,
        BOND_MISSING,
        ANDROID_FORGET_REQUIRED,
        "secure_bond_failed",
        "repair_device_absent",
        "repair_scan_failed",
    )

    fun shouldOfferRepair(code: String?): Boolean = code in retryableCodes

    fun requiresAndroidForget(code: String?): Boolean = code == ANDROID_FORGET_REQUIRED

    /** Repair reuses a saved authorization, so firmware controller capacity is irrelevant. */
    fun availableForSavedController(hasSavedController: Boolean, controllerCount: Int): Boolean {
        require(controllerCount >= 0)
        return hasSavedController
    }
}

/** An explicit Forget may delete an unusable local root after Kitsu rejects it. */
internal object ControllerForgetPolicy {
    const val AUTHORIZATION_REJECTED = "controller_authorization_rejected"

    fun controllerAlreadyAbsent(connectionDetail: String?): Boolean =
        connectionDetail == AUTHORIZATION_REJECTED
}
