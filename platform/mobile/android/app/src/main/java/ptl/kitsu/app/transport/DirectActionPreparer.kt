package ptl.kitsu.app.transport

import ptl.kitsu.app.model.ActionApplyBody
import ptl.kitsu.app.model.ActionCommand
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.toDirectApplyBody

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
