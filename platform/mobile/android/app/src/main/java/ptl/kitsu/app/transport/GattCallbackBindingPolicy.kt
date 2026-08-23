package ptl.kitsu.app.transport

import java.util.UUID

/** Rejects callbacks from a closed/replaced GATT or the wrong characteristic. */
internal object GattCallbackBindingPolicy {
    fun accepts(activeGatt: Any?, callbackGatt: Any, expected: UUID, actual: UUID): Boolean =
        activeGatt === callbackGatt && expected == actual
}
