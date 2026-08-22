package app.kitsu.mobile

import app.kitsu.mobile.cache.CachePolicy
import app.kitsu.mobile.cache.CacheSnapshot
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.ActionPolicy
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.ui.locationSettingsActionState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PolicyAndStorageTest {
    private val validPublicKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    @Test fun messageLimitCountsUtf8Bytes() {
        val command = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000001",
            targetId = validPublicKey,
            text = "🦊".repeat(33),
            messageRoute = MessageRoute.DIRECT,
        )
        assertEquals("text_too_long", ActionPolicy.validate(command))
    }

    @Test fun messageRecipientContractRejectsAliasesAndNonCanonicalChannels() {
        val base = ActionCommand(
            ActionKind.SEND_MESSAGE,
            "00000000-0000-0000-0000-000000000001",
            targetId = "peer-1",
            text = "hello",
            messageRoute = MessageRoute.DIRECT,
        )
        assertEquals("invalid_peer_public_key", ActionPolicy.validate(base))
        assertEquals(
            "invalid_channel_slot",
            ActionPolicy.validate(base.copy(targetId = "04", messageRoute = MessageRoute.CHANNEL)),
        )
        assertNull(ActionPolicy.validate(base.copy(targetId = "0", messageRoute = MessageRoute.CHANNEL)))
        assertNull(ActionPolicy.validate(base.copy(targetId = validPublicKey)))
    }

    @Test fun logsRedactEveryCredentialClass() {
        val rendered = SafeLog.render(
            "event",
            mapOf(
                "access_token" to "raw",
                "privateKey" to "hunter2",
                "pairingDevice" to "private-device",
                "secret" to "correct horse battery staple",
                "count" to 2,
            ),
        )
        assertFalse(rendered.contains("raw"))
        assertFalse(rendered.contains("hunter2"))
        assertFalse(rendered.contains("private-device"))
        assertFalse(rendered.contains("correct horse battery staple"))
        assertTrue(rendered.contains("count=2"))
    }

    @Test fun cacheIsBoundedWithoutDatabase() {
        val entries = (0..300).map {
            HistoryEntry("$it", "c:$it", "test", "entry", it.toLong())
        }
        val bounded = CachePolicy.bounded(CacheSnapshot(history = entries, writtenAt = 1))
        assertEquals(CachePolicy.MAX_HISTORY, bounded.history.size)
        assertEquals("45", bounded.history.first().id)
    }

    @Test fun legacyBleLocationFailuresExposeOtaLockedRecovery() {
        val disabled = locationSettingsActionState("location_services_disabled", null, updateBusy = false)
        assertTrue(disabled.visible)
        assertTrue(disabled.enabled)

        val unavailable = locationSettingsActionState("disconnected", "location_services_unavailable", updateBusy = false)
        assertTrue(unavailable.visible)
        assertTrue(unavailable.enabled)

        val updating = locationSettingsActionState("location_services_disabled", null, updateBusy = true)
        assertTrue(updating.visible)
        assertFalse(updating.enabled)

        assertFalse(locationSettingsActionState("bluetooth_disabled", null, updateBusy = false).visible)
    }
}
