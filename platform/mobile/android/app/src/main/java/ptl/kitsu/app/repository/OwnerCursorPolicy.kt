package ptl.kitsu.app.repository

import ptl.kitsu.app.transport.ConnectionMode

internal data class OwnerCursorNamespace(
    val mode: ConnectionMode,
    val deviceId: String,
)

/** Keeps unrelated Kitsu cursor spaces from crossing saved-device selections. */
internal object OwnerCursorPolicy {
    /** Message IDs are stable while their delivery state mutates; always read the 24-row ring. */
    fun messagesAfter(): String? = null

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
