package ptl.kitsu.app.ui

import android.os.SystemClock
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Hub
import androidx.compose.material.icons.filled.Radio
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material.icons.filled.Tag
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.repository.OwnerState
import kotlinx.coroutines.delay

@Composable
internal fun KitsuMeshScreen(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    modifier: Modifier = Modifier,
) {
    val mesh = owner.status?.mesh
    var advertiseScope by rememberSaveable { mutableStateOf(AdvertiseScope.NEARBY) }
    var cooldownRemainingMs by remember { mutableStateOf(mesh?.advertiseRetryAfterMs ?: 0L) }

    LaunchedEffect(mesh?.advertiseRetryAfterMs) {
        val authoritativeRemaining = mesh?.advertiseRetryAfterMs ?: 0L
        cooldownRemainingMs = authoritativeRemaining
        if (authoritativeRemaining <= 0L) return@LaunchedEffect
        val deadline = SystemClock.elapsedRealtime() + authoritativeRemaining
        while (cooldownRemainingMs > 0L) {
            delay(250L)
            cooldownRemainingMs = (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(0L)
        }
        if (owner.connection.connected && !updateBusy) viewModel.refresh()
    }
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-mesh"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Mesh",
                supporting = "Nearby communication owned by your Kitsu—no gateway and no internet.",
            )
        }

        if (mesh != null) item {
            KitsuCard(title = "Local mesh radio", modifier = Modifier.testTag("mesh-radio-card")) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Row(
                        Modifier.weight(1f),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(
                            Icons.Default.Radio,
                            contentDescription = null,
                            tint = if (mesh.enabled) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                        Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                            Text(
                                if (mesh.enabled) "Radio enabled" else "Radio disabled",
                                style = MaterialTheme.typography.titleMedium,
                            )
                            Text(
                                if (!owner.connection.connected) {
                                    "Connect to change this setting"
                                } else if (owner.meshConfigurationInFlight) {
                                    "Applying on your Kitsu…"
                                } else {
                                    "Stored directly on the selected Kitsu"
                                },
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                    if (owner.meshConfigurationInFlight) {
                        CircularProgressIndicator(modifier = Modifier.padding(10.dp))
                    } else {
                        Switch(
                            checked = mesh.enabled,
                            onCheckedChange = viewModel::configureMesh,
                            enabled = owner.connection.connected && !updateBusy,
                            modifier = Modifier.testTag("mesh-enabled-switch"),
                        )
                    }
                }
                if (updateBusy) {
                    Text(
                        UPDATE_LOCKED_COPY,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        if (!owner.connection.connected) {
            item {
                StatePanel(
                    title = "Connect to see the live mesh",
                    message = "Cached peers may remain visible, but radio state and new activity require Bluetooth.",
                    testTag = "mesh-offline",
                )
            }
        } else if (owner.loading && owner.status == null) {
            item {
                StatePanel(
                    title = "Loading mesh state",
                    message = "Reading radio readiness and nearby identities.",
                    kind = StatePanelKind.LOADING,
                    testTag = "mesh-loading",
                )
            }
        }

        if (owner.errorCode != null) {
            item {
                StatePanel(
                    title = "Mesh state could not refresh",
                    message = owner.errorCode.humanized(),
                    kind = StatePanelKind.ERROR,
                    actionLabel = if (owner.connection.connected && !updateBusy) "Try again" else null,
                    onAction = if (owner.connection.connected && !updateBusy) ({ viewModel.refresh() }) else null,
                    testTag = "mesh-error",
                )
            }
        }

        if (owner.connection.connected && mesh != null && !mesh.timeValid) {
            item {
                StatePanel(
                    title = "Kitsu time is not synchronized",
                    message = "Synchronize Kitsu over the authenticated Bluetooth session before sending time-bound mesh actions.",
                    kind = StatePanelKind.ERROR,
                    actionLabel = if (!updateBusy) "Synchronize time" else null,
                    onAction = if (!updateBusy) ({ viewModel.synchronizeClock() }) else null,
                    testTag = "mesh-time-unset",
                )
            }
        }

        if (mesh != null) {
            item {
                AdvertiseCard(
                    owner = owner,
                    scope = advertiseScope,
                    onScopeChange = { advertiseScope = it },
                    cooldownRemainingMs = cooldownRemainingMs,
                    updateBusy = updateBusy,
                    onAdvertise = { viewModel.advertiseOnce(advertiseScope) },
                    onRefresh = { viewModel.refresh() },
                )
            }
        }

        item {
            SectionHeading(
                title = "Nearby peers",
                supporting = "Authenticated mesh identities recently heard by this Kitsu.",
            )
        }
        if (owner.peers.isEmpty()) {
            item {
                StatePanel(
                    title = "No peers heard yet",
                    message = "Nearby Kitsus will appear after their authenticated mesh traffic is received.",
                    testTag = "mesh-peers-empty",
                )
            }
        } else {
            items(owner.peers, key = { it.id }) { peer ->
                KitsuCard {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Default.Hub, contentDescription = null, tint = MaterialTheme.colorScheme.secondary)
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                            Text(
                                peer.name,
                                style = MaterialTheme.typography.titleMedium,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            Text(
                                peer.route ?: peer.role.humanized(),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        StatusPill("Nearby", StatusTone.POSITIVE)
                    }
                }
            }
        }

        item {
            SectionHeading(
                title = "Channels",
                supporting = "Channel slots configured locally on this Kitsu.",
            )
        }
        if (owner.channels.isEmpty()) {
            item {
                StatePanel(
                    title = "No channel data",
                    message = if (owner.connection.connected) {
                        "This firmware did not report any configured channel slots."
                    } else {
                        "Connect your Kitsu to read its channel configuration."
                    },
                    testTag = "mesh-channels-empty",
                )
            }
        } else {
            items(owner.channels, key = { it.slot }) { channel ->
                val routing = ChannelRoutingPresentationPolicy.present(channel.regionScope)
                KitsuCard {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Default.Tag, contentDescription = null, tint = MaterialTheme.colorScheme.tertiary)
                        Column(Modifier.weight(1f)) {
                            Text(channel.name ?: "Channel ${channel.slot}", style = MaterialTheme.typography.titleMedium)
                            Text(
                                "Slot ${channel.slot}",
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                routing.label,
                                style = MaterialTheme.typography.labelLarge,
                                color = MaterialTheme.colorScheme.primary,
                                modifier = Modifier.testTag("mesh-channel-routing-${channel.slot}"),
                            )
                            Text(
                                routing.detail,
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        StatusPill(
                            if (channel.configured) "Configured" else "Not configured",
                            if (channel.configured) StatusTone.POSITIVE else StatusTone.NEUTRAL,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun AdvertiseCard(
    owner: OwnerState,
    scope: AdvertiseScope,
    onScopeChange: (AdvertiseScope) -> Unit,
    cooldownRemainingMs: Long,
    updateBusy: Boolean,
    onAdvertise: () -> Unit,
    onRefresh: () -> Unit,
) {
    val mesh = requireNotNull(owner.status?.mesh)
    val resultUnknown = mesh.advertiseError == "advertise_result_unknown"
    val ready = owner.connection.connected && mesh.advertiseSupported && mesh.advertiseReady &&
        cooldownRemainingMs <= 0L && !owner.meshAdvertisementInFlight && !updateBusy
    val canRefreshUnknownResult = resultUnknown && owner.connection.connected &&
        !owner.meshAdvertisementInFlight && !updateBusy
    val cooldownSeconds = ((cooldownRemainingMs + 999L) / 1_000L).coerceAtLeast(0L)
    KitsuCard(title = "Advertise now", modifier = Modifier.testTag("advertise-card")) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.Sensors, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f)) {
                Text(
                    when {
                        owner.meshAdvertisementInFlight -> "Queuing signed advertisement"
                        resultUnknown -> "Advertisement result unknown"
                        !mesh.advertiseSupported -> "Firmware update required"
                        mesh.advertiseReady && cooldownRemainingMs <= 0L -> "Ready to advertise"
                        cooldownRemainingMs > 0L -> "Ready again in ${cooldownSeconds}s"
                        else -> advertiseStatusCopy(mesh.advertiseError)
                    },
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    "A fresh signed action is created only when you tap the enabled button.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            when {
                owner.meshAdvertisementInFlight -> CircularProgressIndicator()
                resultUnknown -> StatusPill("Check status", StatusTone.ACTIVE)
                ready -> StatusPill("Ready", StatusTone.POSITIVE)
                cooldownRemainingMs > 0L -> StatusPill("Cooldown", StatusTone.ACTIVE)
                else -> StatusPill("Unavailable", StatusTone.NEUTRAL)
            }
        }

        if (cooldownRemainingMs > 0L) {
            LinearProgressIndicator(
                progress = { (1f - cooldownRemainingMs.coerceAtMost(30_000L) / 30_000f).coerceIn(0f, 1f) },
                modifier = Modifier.fillMaxWidth().testTag("advertise-cooldown"),
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            FilterChip(
                selected = scope == AdvertiseScope.NEARBY,
                onClick = { onScopeChange(AdvertiseScope.NEARBY) },
                label = { Text("Nearby") },
                enabled = !owner.meshAdvertisementInFlight && !updateBusy,
                modifier = Modifier.testTag("advertise-scope-nearby"),
            )
            FilterChip(
                selected = scope == AdvertiseScope.MESH,
                onClick = { onScopeChange(AdvertiseScope.MESH) },
                label = { Text("Mesh-wide") },
                enabled = !owner.meshAdvertisementInFlight && !updateBusy,
                modifier = Modifier.testTag("advertise-scope-mesh"),
            )
        }
        Text(
            if (scope == AdvertiseScope.NEARBY) {
                "Nearby sends one zero-hop advertisement to Kitsus within radio range. " +
                    "It is not repeated, so repeat evidence does not apply."
            } else {
                "Mesh-wide allows the signed advertisement to flood through the local mesh."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(
            onClick = if (resultUnknown) onRefresh else onAdvertise,
            enabled = ready || canRefreshUnknownResult,
            modifier = Modifier.fillMaxWidth().testTag("advertise-now"),
        ) {
            Icon(Icons.Default.Sensors, contentDescription = null)
            androidx.compose.foundation.layout.Spacer(Modifier.padding(4.dp))
            Text(
                when {
                    owner.meshAdvertisementInFlight -> "Queuing…"
                    resultUnknown -> "Refresh status"
                    else -> "Advertise now"
                },
            )
        }
        if (!mesh.advertiseSupported) {
            Text(
                "Install firmware that reports the authenticated advertise capability before using this control.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else if (!mesh.identityReady) {
            Text(
                "The mesh identity is not ready.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        } else if (mesh.advertiseError != null && cooldownRemainingMs <= 0L) {
            Text(
                advertiseStatusCopy(mesh.advertiseError),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.testTag("advertise-error"),
            )
        }
        when (scope) {
            AdvertiseScope.NEARBY -> LastNearbyAdvertBlock(mesh.lastNearbyAdvert)
            AdvertiseScope.MESH -> LastFloodAdvertBlock(mesh.lastFloodAdvert, owner.peers)
        }
    }
}

@Composable
private fun LastNearbyAdvertBlock(evidence: LastNearbyAdvert?) {
    val presentation = NearbyAdvertPresentationPolicy.present(evidence)
    AdvertisementEvidenceBlock(
        title = "Last Nearby advertisement",
        testTag = "advertise-last-nearby",
        statusTag = "advertise-last-nearby-status",
        presentation = presentation,
    )
}

@Composable
private fun LastFloodAdvertBlock(evidence: LastFloodAdvert?, peers: List<ptl.kitsu.app.model.Peer>) {
    val presentation = AdvertEvidencePresentationPolicy.present(evidence, peers)
    AdvertisementEvidenceBlock(
        title = "Last Mesh-wide advertisement",
        testTag = "advertise-last-flood",
        statusTag = "advertise-last-flood-status",
        presentation = presentation,
        extraLine = evidence?.let { RepeatSourcePresentationPolicy.visibleLine(it, peers) },
        extraAccessibilityDetail = evidence?.let { RepeatSourcePresentationPolicy.detail(it, peers) },
    )
}

@Composable
private fun AdvertisementEvidenceBlock(
    title: String,
    testTag: String,
    statusTag: String,
    presentation: AdvertEvidencePresentation,
    extraLine: String? = null,
    extraAccessibilityDetail: String? = null,
) {
    Surface(
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 1.dp,
        modifier = Modifier.fillMaxWidth()
            .testTag(testTag)
            .semantics(mergeDescendants = true) {
                contentDescription = "$title. ${presentation.statusLine}. " +
                    listOfNotNull(extraLine, presentation.detail, extraAccessibilityDetail)
                        .joinToString(". ")
            },
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    presentation.statusLine,
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.weight(1f).testTag(statusTag),
                )
                StatusPill(presentation.badge, presentation.tone)
            }
            extraLine?.let { line ->
                Text(
                    line,
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.testTag("advertise-repeat-source"),
                )
            }
            Text(
                presentation.detail,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun advertiseStatusCopy(error: String?): String = when (error) {
    null -> "Checking readiness"
    "advertise_cooldown" -> "Waiting for the signed-advertisement cooldown"
    "advertise_result_unknown" ->
        "Kitsu may have queued this advertisement, but its signed receipt was lost. Refresh status before trying again."
    "mesh_disabled" -> "Enable the local mesh radio first"
    "mesh_identity_unavailable" -> "Mesh identity is not ready"
    "mesh_radio_unavailable" -> "The mesh radio is unavailable"
    "time_unset" -> "Kitsu time must synchronize first"
    "tx_policy_locked" -> "Transmission policy is locked"
    "send_busy", "queue_full" -> "The outbound mesh queue is busy"
    "location_unavailable" -> "A current one-shot location is unavailable"
    "companion_unavailable" -> "The Kitsu companion pack is unavailable"
    "idempotency_unavailable" -> "Durable action storage is unavailable"
    else -> error.humanized()
}
