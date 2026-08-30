package ptl.kitsu.app.security

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.ActionKind

class ControllerAccessPolicyTest {
    @Test fun caretakerMatchesFirmwareReadFocusWalkAndCarePermissions() {
        val role = ControllerRole.CARETAKER

        assertTrue(ControllerAccessPolicy.allowsOperation(role, "state.get"))
        assertTrue(ControllerAccessPolicy.allowsOperation(role, "history.get"))
        assertTrue(ControllerAccessPolicy.allowsOperation(role, "companion.profile.get.v1"))
        assertTrue(ControllerAccessPolicy.allowsOperation(role, "companion.presentation.read.v1"))
        assertTrue(ControllerAccessPolicy.allowsOperation(role, "focus.start.v1"))
        assertTrue(ControllerAccessPolicy.allowsOperation(role, "adventure.walk.sync.v1"))
        assertTrue(ControllerAccessPolicy.allowsAction(role, ActionKind.PET))
        assertTrue(ControllerAccessPolicy.allowsAction(role, ActionKind.FEED))
        assertTrue(ControllerAccessPolicy.allowsAction(role, ActionKind.PLAY))
    }

    @Test fun caretakerCannotSeeOrInvokeOwnerOnlyCapabilities() {
        val role = ControllerRole.CARETAKER

        listOf(
            "messages.get.v4",
            "messages.mark_read",
            "peers.get",
            "channels.get.v2",
            "mesh.configure",
            "encounter.codes.get.v1",
            "companion.profile.nickname.set.v1",
            "companion.request.answer.v1",
            "adventure.privacy.set.v1",
            "controller.forget",
            "firmware.update.status",
            "firmware.update.begin",
            "firmware.update.write",
            "firmware.update.finish",
            "firmware.update.reboot",
            "firmware.update.abort",
        ).forEach { assertFalse(it, ControllerAccessPolicy.allowsOperation(role, it)) }
        assertFalse(ControllerAccessPolicy.allowsAction(role, ActionKind.LISTEN_ONCE))
        assertFalse(ControllerAccessPolicy.allowsAction(role, ActionKind.ADVERTISE_ONCE))
        assertFalse(ControllerAccessPolicy.allowsAction(role, ActionKind.SEND_MESSAGE))
        assertFalse(ControllerAccessPolicy.allowsOperation(role, "future.owner.operation"))
    }

    @Test fun ownerStillFailsClosedForUnknownOperations() {
        assertTrue(ControllerAccessPolicy.allowsOperation(ControllerRole.OWNER, "messages.get.v4"))
        assertTrue(ControllerAccessPolicy.allowsAction(ControllerRole.OWNER, ActionKind.SEND_MESSAGE))
        assertFalse(
            ControllerAccessPolicy.allowsOperation(
                ControllerRole.OWNER,
                "future.unclassified.operation",
            ),
        )
    }

    @Test fun caretakerCannotSeeTheCrossDeviceEncounterVault() {
        assertFalse(ControllerAccessPolicy.canViewEncounterVault(ControllerRole.CARETAKER))
        assertTrue(ControllerAccessPolicy.canViewEncounterVault(ControllerRole.OWNER))
        assertTrue(ControllerAccessPolicy.canViewEncounterVault(null))
    }
}
