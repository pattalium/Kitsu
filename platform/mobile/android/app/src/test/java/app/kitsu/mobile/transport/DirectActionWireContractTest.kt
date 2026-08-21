package app.kitsu.mobile.transport

import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.model.toDirectApplyBody
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectActionWireContractTest {
    private val json = Json { explicitNulls = false }

    @Test fun directWireUsesAbsoluteUint32DeadlineAndNeverRelativeExpiry() {
        val body = ActionCommand(
            kind = ActionKind.PET,
            clientRequestId = "00000000-0000-0000-0000-000000000001",
            expiresInMs = 30_001,
        ).toDirectApplyBody(nowEpochSeconds = 1_787_000_000)
        val encoded = json.encodeToString(body)
        val root = json.parseToJsonElement(encoded).jsonObject

        assertEquals(1_787_000_031, root.getValue("expires_at_epoch").jsonPrimitive.content.toLong())
        assertFalse("expires_in_ms" in root)
        assertEquals("pet", root.getValue("kind").jsonPrimitive.content)
    }

    @Test fun messageWireKeepsExactPeerAndChannelReferencesInsideParams() {
        val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        val direct = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000001",
            targetId = peer,
            text = "hello",
            messageRoute = MessageRoute.DIRECT,
        ).toDirectApplyBody(1_787_000_000)
        assertEquals(peer, direct.params.getValue("target_id").jsonPrimitive.content)
        assertEquals("direct", direct.params.getValue("route").jsonPrimitive.content)

        val channel = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000002",
            targetId = "0",
            text = "public hello",
            messageRoute = MessageRoute.CHANNEL,
        ).toDirectApplyBody(1_787_000_000)
        assertEquals("0", channel.params.getValue("target_id").jsonPrimitive.content)
        assertEquals("channel", channel.params.getValue("route").jsonPrimitive.content)
    }

    @Test fun directDeadlineRejectsClockOrDeadlineOutsideUint32() {
        assertTrue(
            runCatching {
                ActionCommand(
                    ActionKind.PET,
                    "00000000-0000-0000-0000-000000000001",
                ).toDirectApplyBody(0)
            }.isFailure,
        )
        assertTrue(
            runCatching {
                ActionCommand(
                    ActionKind.PET,
                    "00000000-0000-0000-0000-000000000001",
                ).toDirectApplyBody(4_294_967_290L)
            }.isFailure,
        )
    }
}
