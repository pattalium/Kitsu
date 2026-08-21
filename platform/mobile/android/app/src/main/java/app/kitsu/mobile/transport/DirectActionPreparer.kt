package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionApplyBody
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.toDirectApplyBody

/** Enforces the clock-before-deadline ordering required by the direct firmware contract. */
internal class DirectActionPreparer(
    private val synchronizeClock: suspend () -> Unit,
    private val currentEpochSeconds: () -> Long,
    private val messageOneShotReady: () -> Boolean,
) {
    suspend fun prepare(command: ActionCommand): ActionApplyBody {
        command.requireAllowed()
        synchronizeClock()
        if (command.kind == ActionKind.SEND_MESSAGE && !messageOneShotReady()) {
            throw TransportException("mesh_one_shot_not_ready")
        }
        return try {
            command.toDirectApplyBody(currentEpochSeconds())
        } catch (failure: IllegalArgumentException) {
            throw TransportException(failure.message ?: "invalid_expiry", failure)
        }
    }
}
