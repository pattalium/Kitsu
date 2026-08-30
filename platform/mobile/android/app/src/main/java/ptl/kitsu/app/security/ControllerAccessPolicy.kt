package ptl.kitsu.app.security

import ptl.kitsu.app.model.ActionKind

/**
 * Android mirror of the firmware's closed controller-permission table.
 * Unknown future operations fail closed until both sides classify them.
 */
object ControllerAccessPolicy {
    private val caretakerOperations = setOf(
        "state.get",
        "history.get",
        "companion.profile.get.v1",
        "companion.presentation.open.v1",
        "companion.presentation.read.v1",
        "companion.presentation.close.v1",
        "focus.state.get.v1",
        "focus.start.v1",
        "focus.stop.v1",
        "focus.cancel.v1",
        "focus.ack.v1",
        "adventure.state.get.v1",
        "adventure.walk.start.v1",
        "adventure.walk.sync.v1",
        "adventure.walk.location.v1",
        "adventure.walk.decide.v1",
        "adventure.walk.finish.v1",
        "adventure.walk.ack.v1",
        "action.apply",
    )

    private val ownerOnlyOperations = setOf(
        "peers.get",
        "messages.get",
        "messages.get.v2",
        "messages.get.v3",
        "messages.get.v4",
        "messages.mark_read",
        "encounter.codes.get.v1",
        "encounter.neighbors.get.v1",
        "encounter.neighbor.action.v1",
        "encounter.catalog.get.v1",
        "encounter.discovery.get.v1",
        "companion.profile.nickname.set.v1",
        "companion.request.answer.v1",
        "companion.question.answer.v1",
        "adventure.privacy.set.v1",
        "adventure.home.set.v1",
        "fun.state.get.v1",
        "fun.expedition.start.v1",
        "fun.expedition.claim.v1",
        "fun.story.start.v1",
        "fun.story.advance.v1",
        "fun.story.choose.v1",
        "fun.party.scan.v1",
        "fun.party.host.v1",
        "fun.party.join.v1",
        "fun.party.begin.v1",
        "fun.party.choose.v1",
        "fun.party.leave.v1",
        "channels.get",
        "channels.get.v2",
        "chat.storage.get",
        "clock.sync",
        "mesh.configure",
        "controller.forget",
        "firmware.update.status",
        "firmware.update.begin",
        "firmware.update.write",
        "firmware.update.finish",
        "firmware.update.reboot",
        "firmware.update.abort",
    )

    private val knownOperations = caretakerOperations + ownerOnlyOperations
    private val caretakerActions = setOf(ActionKind.PET, ActionKind.FEED, ActionKind.PLAY)

    fun allowsOperation(role: ControllerRole, operation: String): Boolean =
        operation in knownOperations &&
            (role == ControllerRole.OWNER || operation in caretakerOperations)

    fun allowsAction(role: ControllerRole, action: ActionKind): Boolean =
        role == ControllerRole.OWNER || action in caretakerActions

    fun isOwner(role: ControllerRole): Boolean = role == ControllerRole.OWNER

    /** The encounter vault can contain ownership records from other owner-authorized Kitsu. */
    fun canViewEncounterVault(role: ControllerRole?): Boolean =
        role != ControllerRole.CARETAKER
}
