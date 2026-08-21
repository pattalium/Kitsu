package app.kitsu.mobile.repository

import app.kitsu.mobile.transport.ConnectionMode

internal data class OwnerCursorNamespace(
    val mode: ConnectionMode,
    val deviceId: String,
)

/** Keeps unrelated backend/device cursor spaces from crossing transports. */
internal object OwnerCursorPolicy {
    fun resume(
        cursor: String?,
        previous: OwnerCursorNamespace?,
        current: OwnerCursorNamespace,
    ): String? = cursor.takeIf { previous == current }

    fun shouldReplace(
        previous: OwnerCursorNamespace?,
        current: OwnerCursorNamespace,
        cursorExpired: Boolean,
    ): Boolean = previous != current || cursorExpired
}
