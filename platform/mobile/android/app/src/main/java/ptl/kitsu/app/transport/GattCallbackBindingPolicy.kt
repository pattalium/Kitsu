package ptl.kitsu.app.transport

import java.util.UUID

/** Rejects callbacks from a closed/replaced GATT or the wrong characteristic. */
internal object GattCallbackBindingPolicy {
    fun accepts(activeGatt: Any?, callbackGatt: Any): Boolean = activeGatt === callbackGatt

    fun accepts(activeGatt: Any?, callbackGatt: Any, expected: UUID, actual: UUID): Boolean =
        accepts(activeGatt, callbackGatt) && expected == actual
}

/** Maps Android's numeric GATT failures to stable, actionable app diagnostics. */
internal object GattStatusPolicy {
    fun connectionFailure(status: Int): String = when (status) {
        // Android reports 0x13 when the peripheral terminates a link whose SMP
        // keys no longer match. Retrying the same cached bond cannot repair it.
        0x13 -> "bluetooth_pairing_repair_required"
        else -> "gatt_status_$status"
    }

    fun notificationSubscriptionFailure(status: Int): String = when (status) {
        // A secured CCCD can return either ATT error while Android is holding a
        // missing/stale SMP bond. Never expose those as an opaque descriptor error.
        0x05, 0x0f -> "bluetooth_pairing_repair_required"
        else -> "notify_descriptor_write_failed"
    }
}
