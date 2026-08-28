package ptl.kitsu.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.ExpeditionStatus
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.DiscoveredPartyHost
import ptl.kitsu.app.model.PartyPhase
import ptl.kitsu.app.model.PartyRole
import ptl.kitsu.app.model.PartySignalChoice
import ptl.kitsu.app.model.StoryStatus
import ptl.kitsu.app.repository.OwnerState

@Composable
internal fun KitsuFunCards(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
) {
    if (!owner.funSupported) return
    val state = owner.funState
    val enabled = owner.connection.connected && !updateBusy && !owner.funMutationInFlight

    SectionHeading(
        title = "Adventures",
        supporting = "Offline expeditions, short personality stories, and local radio parties.",
        modifier = Modifier.testTag("fun-heading"),
    )

    if (owner.funErrorCode != null || state == null) {
        KitsuCard(modifier = Modifier.testTag("fun-error")) {
            Text("Adventures could not refresh", style = MaterialTheme.typography.titleMedium)
            Text(
                owner.funErrorCode?.humanized() ?: "No current adventure state is available.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedButton(onClick = viewModel::refresh, enabled = enabled) { Text("Try again") }
        }
        return
    }

    ExpeditionCard(state, enabled, viewModel)
    StoryCard(state, enabled, viewModel)
    PartyCard(state, enabled, viewModel)
}

@Composable
private fun ExpeditionCard(state: FunState, enabled: Boolean, viewModel: MainViewModel) {
    val expedition = state.expedition
    KitsuCard(title = "Expedition", modifier = Modifier.testTag("expedition-card")) {
        when (expedition.status) {
            ExpeditionStatus.IDLE -> {
                Text(
                    "Send your companion out while the device is in your pocket. No items or inventory required.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ExpeditionButton("15 min", enabled, Modifier.weight(1f)) {
                        viewModel.startExpedition(ExpeditionDuration.SHORT)
                    }
                    ExpeditionButton("2 hr", enabled, Modifier.weight(1f)) {
                        viewModel.startExpedition(ExpeditionDuration.MEDIUM)
                    }
                    ExpeditionButton("8 hr", enabled, Modifier.weight(1f)) {
                        viewModel.startExpedition(ExpeditionDuration.LONG)
                    }
                }
            }

            ExpeditionStatus.SCOUTING -> {
                Text("${expedition.duration.label()} expedition in progress")
                LinearProgressIndicator(
                    progress = { expedition.progressPercent.coerceIn(0, 100) / 100f },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    "${formatRemaining(expedition.remainingSeconds)} remaining · ${expedition.progressPercent}%",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedButton(onClick = viewModel::refresh, enabled = enabled) { Text("Refresh") }
            }

            ExpeditionStatus.RETURNED -> {
                Text(expedition.report?.headline ?: "Expedition complete", style = MaterialTheme.typography.titleMedium)
                expedition.report?.detail?.let {
                    Text(it, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Button(onClick = viewModel::claimExpedition, enabled = enabled) { Text("Claim report") }
            }
        }
    }
}

@Composable
private fun ExpeditionButton(
    label: String,
    enabled: Boolean,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    OutlinedButton(onClick = onClick, enabled = enabled, modifier = modifier) { Text(label) }
}

@Composable
private fun StoryCard(state: FunState, enabled: Boolean, viewModel: MainViewModel) {
    val story = state.story
    KitsuCard(title = "Personality story", modifier = Modifier.testTag("story-card")) {
        story.resolution?.let { resolution ->
            Text(resolution.line1, style = MaterialTheme.typography.titleMedium)
            if (resolution.line2.isNotEmpty()) Text(resolution.line2)
            Text(
                "A ${resolution.tone.name.lowercase()} memory",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        when (story.status) {
            StoryStatus.IDLE -> {
                if (story.resolution == null) {
                    Text(
                        "Two quick scenes and one choice, shaped by your companion's personality.",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Button(onClick = { viewModel.startStory() }, enabled = enabled) {
                    Text(if (story.resolution == null) "Begin a story" else "Another story")
                }
            }

            StoryStatus.READING -> {
                val beat = story.beat ?: return@KitsuCard
                Text(beat.line1, style = MaterialTheme.typography.titleMedium)
                if (beat.line2.isNotEmpty()) Text(beat.line2)
                Button(onClick = { viewModel.advanceStory(beat.storyId) }, enabled = enabled) {
                    Text("Continue")
                }
            }

            StoryStatus.CHOOSING -> {
                val beat = story.beat ?: return@KitsuCard
                Text(beat.line1, style = MaterialTheme.typography.titleMedium)
                if (beat.line2.isNotEmpty()) Text(beat.line2)
                beat.choices.forEachIndexed { index, choice ->
                    OutlinedButton(
                        onClick = { viewModel.chooseStory(beat.storyId, index) },
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(choice) }
                }
            }
        }
    }
}

@Composable
private fun PartyCard(state: FunState, enabled: Boolean, viewModel: MainViewModel) {
    val party = state.party
    KitsuCard(title = "Party Hotspot", modifier = Modifier.testTag("party-card")) {
        Text(
            "A local LoRa signal hunt for 2–4 Kitsu. New people, bigger groups, and daily meetups earn more Party Bond.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        when (party.phase) {
            PartyPhase.IDLE -> {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = viewModel::scanParty,
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("Find party") }
                    Button(
                        onClick = viewModel::hostParty,
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("Host") }
                }
                DiscoveredPartyHosts(party.discoveredHosts, enabled, viewModel)
            }

            PartyPhase.UNAVAILABLE -> {
                Text("Party radio is unavailable on this Kitsu right now.")
            }

            PartyPhase.JOINING, PartyPhase.LOBBY -> {
                if (party.phase == PartyPhase.JOINING && party.discoveredHosts.isNotEmpty()) {
                    Text("Nearby Party Hotspot found", style = MaterialTheme.typography.titleMedium)
                    DiscoveredPartyHosts(party.discoveredHosts, enabled, viewModel)
                    OutlinedButton(onClick = viewModel::leaveParty, enabled = enabled) { Text("Stop") }
                    return@KitsuCard
                }
                Text(
                    if (party.role == PartyRole.HOST) {
                        "Lobby open · ${party.participantCount}/4 Kitsu"
                    } else if (party.phase == PartyPhase.JOINING) {
                        "Join sent to ${party.hostDeviceId ?: "party"}"
                    } else {
                        "Joined ${party.hostDeviceId ?: "party"} · ${party.participantCount}/4 Kitsu"
                    },
                    style = MaterialTheme.typography.titleMedium,
                )
                if (party.role == PartyRole.HOST) {
                    Button(
                        onClick = viewModel::beginParty,
                        enabled = enabled && party.participantCount >= 2,
                    ) { Text("Start signal hunt") }
                } else {
                    Text("Waiting for the host to begin.")
                }
                OutlinedButton(onClick = viewModel::leaveParty, enabled = enabled) { Text("Leave") }
            }

            PartyPhase.ROUND -> {
                Text("Round ${party.round} of 3", style = MaterialTheme.typography.titleMedium)
                if (party.localChoice == PartySignalChoice.NONE) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        PartyChoiceButton("Sweep", PartySignalChoice.SWEEP, party.round, enabled, viewModel, Modifier.weight(1f))
                        PartyChoiceButton("Listen", PartySignalChoice.LISTEN, party.round, enabled, viewModel, Modifier.weight(1f))
                        PartyChoiceButton("Pulse", PartySignalChoice.PULSE, party.round, enabled, viewModel, Modifier.weight(1f))
                    }
                } else {
                    Text("${party.localChoice.name.lowercase()} locked. Waiting for the party.")
                }
                OutlinedButton(onClick = viewModel::refresh, enabled = enabled) { Text("Refresh") }
            }

            PartyPhase.COMPLETE -> {
                Text(
                    "${party.reward.tier.name.lowercase().replaceFirstChar { it.uppercase() }} signal",
                    style = MaterialTheme.typography.titleMedium,
                )
                Text("Score ${party.reward.score}/${party.reward.maximumScore}")
                Text(
                    "+${party.reward.bondAwarded} Party Bond · total ${party.reward.partyBond} · ${party.reward.currentStreakDays}-day streak",
                    color = MaterialTheme.colorScheme.primary,
                )
                Button(onClick = viewModel::leaveParty, enabled = enabled) { Text("Done") }
            }

            PartyPhase.CANCELLED, PartyPhase.EXPIRED -> {
                Text(if (party.phase == PartyPhase.EXPIRED) "Party expired." else "Party closed.")
                Button(onClick = viewModel::leaveParty, enabled = enabled) { Text("Back") }
            }
        }
    }
}

@Composable
private fun DiscoveredPartyHosts(
    hosts: List<DiscoveredPartyHost>,
    enabled: Boolean,
    viewModel: MainViewModel,
) {
    hosts.forEach { host ->
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(Modifier.weight(1f)) {
                Text(host.hostDeviceId, style = MaterialTheme.typography.titleMedium)
                Text(
                    "${host.participantCount}/4 nearby · ${host.joinWindowSeconds}s left",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            OutlinedButton(onClick = { viewModel.joinParty(host) }, enabled = enabled) {
                Text("Join")
            }
        }
    }
}

@Composable
private fun PartyChoiceButton(
    label: String,
    choice: PartySignalChoice,
    round: Int,
    enabled: Boolean,
    viewModel: MainViewModel,
    modifier: Modifier,
) {
    OutlinedButton(
        onClick = { viewModel.chooseParty(round, choice) },
        enabled = enabled,
        modifier = modifier,
    ) { Text(label) }
}

private fun ExpeditionDuration?.label(): String = when (this) {
    ExpeditionDuration.SHORT -> "15-minute"
    ExpeditionDuration.MEDIUM -> "2-hour"
    ExpeditionDuration.LONG -> "8-hour"
    null -> ""
}

private fun formatRemaining(seconds: Long): String = when {
    seconds >= 3_600L -> "${seconds / 3_600L}h ${(seconds % 3_600L) / 60L}m"
    seconds >= 60L -> "${seconds / 60L}m"
    else -> "${seconds}s"
}
