package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.MessageRoute
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectActionPreparerTest {
    private val requestId = "00000000-0000-0000-0000-000000000001"

    @Test fun everyDirectCareAndMessageActionSynchronizesBeforeDeadline() = runTest {
        val commands = listOf(
            ActionCommand(ActionKind.PET, requestId),
            ActionCommand(ActionKind.FEED, requestId),
            ActionCommand(ActionKind.PLAY, requestId),
            ActionCommand(ActionKind.LISTEN_ONCE, requestId, durationMs = 60_000),
            ActionCommand(
                ActionKind.SEND_MESSAGE,
                requestId,
                targetId = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                text = "hello",
                messageRoute = MessageRoute.DIRECT,
            ),
        )

        commands.forEach { command ->
            val order = mutableListOf<String>()
            val body = DirectActionPreparer(
                synchronizeClock = { order += "clock.sync" },
                currentEpochSeconds = {
                    order += "deadline"
                    1_787_000_000
                },
                messageOneShotReady = { true },
            ).prepare(command)

            assertEquals(listOf("clock.sync", "deadline"), order)
            assertEquals(1_787_000_030, body.expiresAtEpoch)
        }
    }

    @Test fun messageReadinessIsCheckedOnlyAfterClockSync() = runTest {
        val order = mutableListOf<String>()
        val failure = runCatching {
            DirectActionPreparer(
                synchronizeClock = { order += "clock.sync" },
                currentEpochSeconds = {
                    order += "deadline"
                    1_787_000_000
                },
                messageOneShotReady = {
                    order += "ready"
                    false
                },
            ).prepare(
                ActionCommand(
                    ActionKind.SEND_MESSAGE,
                    requestId,
                    targetId = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                    text = "hello",
                    messageRoute = MessageRoute.DIRECT,
                ),
            )
        }.exceptionOrNull()

        assertTrue(failure is TransportException)
        assertEquals("mesh_one_shot_not_ready", (failure as TransportException).code)
        assertEquals(listOf("clock.sync", "ready"), order)
    }
}
