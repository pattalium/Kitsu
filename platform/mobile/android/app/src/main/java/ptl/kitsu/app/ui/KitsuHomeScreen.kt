package ptl.kitsu.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CardGiftcard
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material.icons.filled.Pets
import androidx.compose.material.icons.filled.Restaurant
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.WavingHand
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.R
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.transport.ConnectionMode

@Composable
internal fun KitsuHomeScreen(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    neighborActionsInFlight: Set<String>,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenAppSettings: () -> Unit,
    onManageKitsu: () -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-home"),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            KitsuHeroCard(
                owner = owner,
                updateBusy = updateBusy,
                onConnect = { viewModel.connect() },
                onRefresh = viewModel::refresh,
                onDisconnect = viewModel::disconnect,
                onManageKitsu = onManageKitsu,
            )
        }

        if (owner.errorCode != null) {
            item {
                ConnectionRecoveryPanel(
                    owner = owner,
                    updateBusy = updateBusy,
                    onConnect = { viewModel.connect() },
                    onRequestBlePermissions = onRequestBlePermissions,
                    onEnableBluetooth = onEnableBluetooth,
                    onOpenLocationSettings = onOpenLocationSettings,
                    onOpenAppSettings = onOpenAppSettings,
                )
            }
        }

        when {
            owner.status != null -> {
                item { NeedsAndBondCard(owner) }
                item { CareActionsCard(owner, viewModel, updateBusy) }
            }
            owner.loading -> item {
                StatePanel(
                    title = "Waking your Kitsu",
                    message = "Establishing the private Bluetooth link and loading its latest state.",
                    kind = StatePanelKind.LOADING,
                    testTag = "home-loading",
                )
            }
            else -> item {
                StatePanel(
                    title = if (owner.savedKitsu.isEmpty()) "Meet your Kitsu" else "Your Kitsu is offline",
                    message = if (owner.savedKitsu.isEmpty()) {
                        "Pair a nearby Kitsu from Settings. Your authorization stays on this phone."
                    } else {
                        "Bring the selected Kitsu nearby, make sure it is powered, then connect."
                    },
                    actionLabel = null,
                    onAction = null,
                    testTag = "home-empty",
                )
            }
        }

        if (owner.status != null && owner.nearbyKitsuSupported) {
            item {
                SectionHeading(
                    title = "Nearby Kitsu",
                    supporting = "Owned companions heard by Kitsu directly — separate from MeshCore.",
                    modifier = Modifier.testTag("nearby-kitsu-heading"),
                )
            }
            when {
                owner.nearbyKitsuErrorCode != null -> item {
                    KitsuCard(modifier = Modifier.testTag("nearby-kitsu-error")) {
                        Text("Nearby Kitsu could not refresh", style = MaterialTheme.typography.titleMedium)
                        Text(
                            owner.nearbyKitsuErrorCode.humanized(),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        OutlinedButton(onClick = viewModel::refresh, enabled = !updateBusy) {
                            Text("Try again")
                        }
                    }
                }
                owner.nearbyKitsu.isEmpty() -> item {
                    KitsuCard(modifier = Modifier.testTag("nearby-kitsu-empty")) {
                        Text("No owned Kitsu heard yet", style = MaterialTheme.typography.titleMedium)
                        Text(
                            "Tap Listen above while another Kitsu is nearby. Direct Kitsu signals are not sent through MeshCore repeaters.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                else -> items(
                    items = owner.nearbyKitsu.take(8),
                    key = NearbyKitsu::sessionKey,
                ) { neighbor ->
                    NearbyOwnedKitsuCard(
                        neighbor = neighbor,
                        enabled = owner.connection.connected && !updateBusy,
                        actionInFlight = neighbor.sessionKey in neighborActionsInFlight,
                        supportedActions = owner.nearbyInteractionKinds,
                        onInteraction = { kind -> viewModel.interactWithNeighbor(neighbor, kind) },
                    )
                }
            }
        }

        if (owner.status != null && owner.funSupported) {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    KitsuFunCards(owner, viewModel, updateBusy)
                }
            }
        }

        item {
            SectionHeading(
                title = "Recent moments",
                supporting = "Activity stored locally from your Kitsu and nearby mesh.",
            )
        }
        if (owner.history.isEmpty()) {
            item {
                StatePanel(
                    title = "No moments yet",
                    message = "Care actions and authenticated mesh activity will appear here.",
                    testTag = "history-empty",
                )
            }
        } else {
            items(owner.history.takeLast(6).reversed(), key = { it.id }) { entry ->
                KitsuCard {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.Top,
                    ) {
                        Icon(
                            Icons.Default.Pets,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.primary,
                        )
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                            Text(entry.kind.humanized(), style = MaterialTheme.typography.titleMedium)
                            Text(
                                entry.summary,
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
}

@Composable
private fun NearbyOwnedKitsuCard(
    neighbor: NearbyKitsu,
    enabled: Boolean,
    actionInFlight: Boolean,
    supportedActions: Set<NeighborInteractionKind>,
    onInteraction: (NeighborInteractionKind) -> Unit,
) {
    val creature = nearbyCreaturePresentation(neighbor.packId)
    KitsuCard(modifier = Modifier.testTag("nearby-kitsu-${neighbor.deviceId}")) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            NearbyKitsuPortrait(creature, Modifier.size(76.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(
                    creature.name,
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "Owned Kitsu · ${neighbor.deviceId}",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                )
                Text(
                    "${nearbyMoodLabel(neighbor.mood)} · ${nearbyStageLabel(neighbor.evolutionStage)}",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    "${nearbySignalLabel(neighbor.rssi)} signal · Seen ${nearbyLastSeenLabel(neighbor.lastSeenAgeMs)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (!creature.known) {
                    Text(
                        "Unknown pack ${neighbor.packId.toString(16).uppercase().padStart(8, '0')}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            NearbyActionButton(
                label = "Pet",
                icon = Icons.Default.Favorite,
                kind = NeighborInteractionKind.PET,
                deviceId = neighbor.deviceId,
                enabled = enabled && !actionInFlight && NeighborInteractionKind.PET in supportedActions,
                onInteraction = onInteraction,
                modifier = Modifier.weight(1f),
            )
            NearbyActionButton(
                label = "Greet",
                icon = Icons.Default.WavingHand,
                kind = NeighborInteractionKind.GREET,
                deviceId = neighbor.deviceId,
                enabled = enabled && !actionInFlight && NeighborInteractionKind.GREET in supportedActions,
                onInteraction = onInteraction,
                modifier = Modifier.weight(1f),
            )
            NearbyActionButton(
                label = "Play",
                icon = Icons.Default.SportsEsports,
                kind = NeighborInteractionKind.PLAY,
                deviceId = neighbor.deviceId,
                enabled = enabled && !actionInFlight && NeighborInteractionKind.PLAY in supportedActions,
                onInteraction = onInteraction,
                modifier = Modifier.weight(1f),
            )
            NearbyActionButton(
                label = "Gift",
                icon = Icons.Default.CardGiftcard,
                kind = NeighborInteractionKind.GIFT,
                deviceId = neighbor.deviceId,
                enabled = enabled && !actionInFlight && NeighborInteractionKind.GIFT in supportedActions,
                onInteraction = onInteraction,
                modifier = Modifier.weight(1f),
            )
        }
        if (actionInFlight) {
            Text(
                "Sending your moment…",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary,
            )
        } else {
            val unavailable = NeighborInteractionKind.entries.filterNot(supportedActions::contains)
            if (unavailable.isNotEmpty()) {
                Text(
                    "${unavailable.joinToString(" and ") { it.actionLabel }} " +
                        "${if (unavailable.size == 1) "needs" else "need"} newer Kitsu firmware.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun NearbyActionButton(
    label: String,
    icon: ImageVector,
    kind: NeighborInteractionKind,
    deviceId: String,
    enabled: Boolean,
    onInteraction: (NeighborInteractionKind) -> Unit,
    modifier: Modifier = Modifier,
) {
    FilledTonalButton(
        onClick = { onInteraction(kind) },
        enabled = enabled,
        modifier = modifier.testTag("nearby-kitsu-${kind.name.lowercase()}-$deviceId"),
        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 10.dp),
    ) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(16.dp))
        Spacer(Modifier.size(4.dp))
        Text(label, maxLines = 1)
    }
}

private val NeighborInteractionKind.actionLabel: String
    get() = when (this) {
        NeighborInteractionKind.PET -> "Pet"
        NeighborInteractionKind.GREET -> "Greet"
        NeighborInteractionKind.PLAY -> "Play"
        NeighborInteractionKind.GIFT -> "Gift"
    }

private fun nearbySignalLabel(rssi: Double): String = when {
    rssi >= -65.0 -> "Strong"
    rssi >= -85.0 -> "Good"
    else -> "Faint"
}

@Composable
private fun KitsuHeroCard(
    owner: OwnerState,
    updateBusy: Boolean,
    onConnect: () -> Unit,
    onRefresh: () -> Unit,
    onDisconnect: () -> Unit,
    onManageKitsu: () -> Unit,
) {
    val status = owner.status
    val selected = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }
    val connection = connectionPresentation(owner)
    val heroGradient = Brush.linearGradient(
        listOf(
            MaterialTheme.colorScheme.primaryContainer,
            MaterialTheme.colorScheme.secondaryContainer,
            MaterialTheme.colorScheme.surface,
        ),
    )
    Card(
        modifier = Modifier.fillMaxWidth().testTag("kitsu-hero"),
        colors = CardDefaults.cardColors(containerColor = Color.Transparent),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Box(
            modifier = Modifier.fillMaxWidth().background(heroGradient).padding(22.dp),
        ) {
            Icon(
                painter = painterResource(R.drawable.kitsu_app_icon_monochrome),
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.12f),
                modifier = Modifier.align(Alignment.CenterEnd).size(156.dp),
            )
            Column(
                modifier = Modifier.fillMaxWidth(0.82f),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(
                    "KITSU",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    status?.companionName ?: selected?.displayName ?: "Your Kitsu",
                    style = MaterialTheme.typography.headlineLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    when {
                        status?.mood.equals("SLEEPING", ignoreCase = true) -> "Sleeping peacefully"
                        status != null -> status.mood.humanized()
                        owner.loading -> "Getting ready"
                        else -> "Ready when you are"
                    },
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                StatusPill(connection.label, connection.tone)
                status?.batteryPercent?.let {
                    Text(
                        "Battery $it%",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Spacer(Modifier.height(2.dp))
                when {
                    owner.connection.connected -> Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        OutlinedButton(
                            onClick = onRefresh,
                            enabled = !owner.loading && !updateBusy,
                            modifier = Modifier.testTag("connection-refresh"),
                        ) { Text("Refresh") }
                        TextButton(
                            onClick = onDisconnect,
                            enabled = !updateBusy,
                            modifier = Modifier.testTag("connection-disconnect"),
                        ) { Text("Disconnect") }
                    }
                    selected != null && owner.errorCode == null -> Button(
                        onClick = onConnect,
                        enabled = !owner.loading && !owner.pairing && !updateBusy,
                        modifier = Modifier.testTag("connection-connect"),
                    ) { Text(if (owner.loading) "Connecting…" else "Connect") }
                    selected == null -> Button(onClick = onManageKitsu, enabled = !updateBusy) {
                        Text("Set up a Kitsu")
                    }
                }
            }
        }
    }
}

@Composable
private fun NeedsAndBondCard(owner: OwnerState) {
    val status = requireNotNull(owner.status)
    KitsuCard(title = "How ${status.companionName} feels") {
        NeedMeter("Energy", status.needs.energy, MaterialTheme.colorScheme.tertiary)
        NeedMeter("Curiosity", status.needs.curiosity, MaterialTheme.colorScheme.secondary)
        NeedMeter("Affection", status.needs.affection, MaterialTheme.colorScheme.primary)
        Spacer(Modifier.height(4.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Column {
                Text("Bond level ${status.bondLevel}", style = MaterialTheme.typography.titleMedium)
                Text(
                    status.evolutionStage?.humanized() ?: "Growing together",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                "${status.bondProgressPercent.coerceIn(0, 100)}%",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        androidx.compose.material3.LinearProgressIndicator(
            progress = { status.bondProgressPercent.coerceIn(0, 100) / 100f },
            modifier = Modifier.fillMaxWidth().height(8.dp).clip(androidx.compose.foundation.shape.CircleShape),
            color = MaterialTheme.colorScheme.primary,
            trackColor = MaterialTheme.colorScheme.surfaceVariant,
        )
    }
}

@Composable
private fun CareActionsCard(owner: OwnerState, viewModel: MainViewModel, updateBusy: Boolean) {
    val enabled = owner.connection.connected && !updateBusy
    KitsuCard(title = "Spend a moment together") {
        Text(
            "Care travels only across your authenticated Bluetooth link.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            CareAction("Pet", Icons.Default.Favorite, enabled, Modifier.weight(1f)) {
                viewModel.simpleAction(ActionKind.PET)
            }
            CareAction("Feed", Icons.Default.Restaurant, enabled, Modifier.weight(1f)) {
                viewModel.simpleAction(ActionKind.FEED)
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            CareAction("Play", Icons.Default.SportsEsports, enabled, Modifier.weight(1f)) {
                viewModel.simpleAction(ActionKind.PLAY)
            }
            CareAction("Listen", Icons.Default.Hearing, enabled, Modifier.weight(1f)) {
                viewModel.simpleAction(ActionKind.LISTEN_ONCE)
            }
        }
        if (!owner.connection.connected) {
            Text(
                "Connect your Kitsu to use care actions.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun CareAction(
    label: String,
    icon: ImageVector,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    FilledTonalButton(onClick = onClick, enabled = enabled, modifier = modifier) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(18.dp))
        Spacer(Modifier.size(8.dp))
        Text(label)
    }
}

@Composable
private fun ConnectionRecoveryPanel(
    owner: OwnerState,
    updateBusy: Boolean,
    onConnect: () -> Unit,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    val locationAction = locationSettingsActionState(owner.connection.detail, owner.errorCode, updateBusy)
    KitsuCard(title = "Connection needs attention") {
        Text(
            connectionPresentation(owner).detail,
            color = MaterialTheme.colorScheme.error,
            style = MaterialTheme.typography.bodyMedium,
        )
        when {
            updateBusy -> Text(UPDATE_LOCKED_COPY, style = MaterialTheme.typography.bodyMedium)
            owner.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    BLE_PERMISSION_COPY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onRequestBlePermissions) { Text("Allow Bluetooth") }
                    TextButton(onClick = onOpenAppSettings) { Text("App settings") }
                }
            }
            owner.connection.detail == "bluetooth_disabled" || owner.errorCode == "bluetooth_disabled" ->
                Button(onClick = onEnableBluetooth) { Text("Turn on Bluetooth") }
            locationAction.visible -> Button(
                onClick = onOpenLocationSettings,
                enabled = locationAction.enabled,
                modifier = Modifier.testTag("connection-location-settings"),
            ) { Text("Location settings") }
            owner.activeDeviceAddress != null -> Button(
                onClick = onConnect,
                modifier = Modifier.testTag("connection-connect"),
            ) { Text("Try again") }
        }
    }
}
