package ptl.kitsu.app.repository

import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.transport.TransportException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withTimeout

internal const val ADVERTISE_SUBMISSION_TIMEOUT_MILLIS = 20_000L
internal const val ADVERTISE_COOLDOWN_MILLIS = 30_000L
internal const val ADVERTISE_COOLDOWN = "advertise_cooldown"
internal const val ADVERTISE_RESULT_UNKNOWN = "advertise_result_unknown"

private val ADVERTISE_OUTCOME_UNKNOWN_TRANSPORT_ERRORS = setOf(
    "request_timeout",
    "gatt_write_failed",
    "gatt_disconnected",
    "disconnected",
    "frame_timeout",
    "response_binding_failed",
    "malformed_response",
    "malformed_action_receipt",
)

/**
 * Bounds the whole submission, including direct clock sync and admission to the
 * transport request mutex. A timeout after the tap is fail-closed because the
 * radio side effect may have happened even when its signed receipt was lost.
 */
internal suspend fun awaitAdvertiseReceipt(
    timeoutMillis: Long,
    submit: suspend () -> ActionReceipt,
): ActionReceipt {
    require(timeoutMillis > 0L) { "advertise_timeout_required" }
    return try {
        withTimeout(timeoutMillis) { submit() }
    } catch (timeout: TimeoutCancellationException) {
        throw TransportException(ADVERTISE_RESULT_UNKNOWN, timeout)
    } catch (cancelled: CancellationException) {
        throw cancelled
    } catch (failure: TransportException) {
        if (failure.code in ADVERTISE_OUTCOME_UNKNOWN_TRANSPORT_ERRORS) {
            throw TransportException(ADVERTISE_RESULT_UNKNOWN, failure)
        }
        throw failure
    }
}
