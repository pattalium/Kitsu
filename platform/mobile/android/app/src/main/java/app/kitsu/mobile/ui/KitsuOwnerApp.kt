package app.kitsu.mobile.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Message
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.kitsu.mobile.MainViewModel
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.security.MAX_SAVED_KITSU
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.update.FirmwareInstallStage
import app.kitsu.mobile.update.locksCompanionControls

private enum class OwnerTab(val label: String) { HOME("Home"), MESSAGES("Messages"), NETWORK("Mesh"), CARE("Care"), SETTINGS("Settings") }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun KitsuOwnerApp(
    viewModel: MainViewModel,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onPairController: (String) -> Unit,
    onFinishPairing: () -> Unit,
    onOpenFirmwarePackage: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    val owner by viewModel.owner.collectAsStateWithLifecycle()
    val notice by viewModel.notice.collectAsStateWithLifecycle()
    val firmware by viewModel.firmware.collectAsStateWithLifecycle()
    val updateBusy = firmware.progress.stage.locksCompanionControls
    var tab by remember { mutableStateOf(OwnerTab.HOME) }
    val snackbar = remember { SnackbarHostState() }

    LaunchedEffect(notice) {
        notice?.let {
            snackbar.showSnackbar(it.humanized())
            viewModel.clearNotice()
        }
    }

    MaterialTheme {
        Scaffold(
            topBar = { TopAppBar(title = { Text("Kitsu · ${tab.label}") }) },
            snackbarHost = { SnackbarHost(snackbar) },
            bottomBar = {
                NavigationBar {
                    NavigationBarItem(tab == OwnerTab.HOME, { tab = OwnerTab.HOME }, { Icon(Icons.Default.Home, null) }, label = { Text("Home") }, modifier = Modifier.testTag("nav-home"))
                    NavigationBarItem(tab == OwnerTab.MESSAGES, { tab = OwnerTab.MESSAGES }, { Icon(Icons.AutoMirrored.Filled.Message, null) }, label = { Text("Messages") }, modifier = Modifier.testTag("nav-messages"))
                    NavigationBarItem(tab == OwnerTab.NETWORK, { tab = OwnerTab.NETWORK }, { Icon(Icons.Default.Share, null) }, label = { Text("Mesh") }, modifier = Modifier.testTag("nav-network"))
                    NavigationBarItem(tab == OwnerTab.CARE, { tab = OwnerTab.CARE }, { Icon(Icons.Default.Favorite, null) }, label = { Text("Care") }, modifier = Modifier.testTag("nav-care"))
                    NavigationBarItem(tab == OwnerTab.SETTINGS, { tab = OwnerTab.SETTINGS }, { Icon(Icons.Default.Settings, null) }, label = { Text("Settings") }, modifier = Modifier.testTag("nav-settings"))
                }
            },
        ) { padding ->
            when (tab) {
                OwnerTab.HOME -> HomeScreen(owner, viewModel, updateBusy, onRequestBlePermissions, onEnableBluetooth, onOpenLocationSettings, onOpenAppSettings, Modifier.padding(padding))
                OwnerTab.MESSAGES -> MessagesScreen(owner, viewModel, updateBusy, Modifier.padding(padding))
                OwnerTab.NETWORK -> MeshScreen(owner, viewModel, updateBusy, Modifier.padding(padding))
                OwnerTab.CARE -> CareScreen(owner, viewModel, updateBusy, Modifier.padding(padding))
                OwnerTab.SETTINGS -> SettingsScreen(
                    owner = owner,
                    firmware = firmware,
                    viewModel = viewModel,
                    onRequestBlePermissions = onRequestBlePermissions,
                    onEnableBluetooth = onEnableBluetooth,
                    onOpenLocationSettings = onOpenLocationSettings,
                    onPairController = onPairController,
                    onFinishPairing = onFinishPairing,
                    onOpenFirmwarePackage = onOpenFirmwarePackage,
                    onOpenAppSettings = onOpenAppSettings,
                    updateBusy = updateBusy,
                    modifier = Modifier.padding(padding),
                )
            }
        }
    }
}

@Composable
private fun HomeScreen(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenAppSettings: () -> Unit,
    modifier: Modifier,
) {
    LazyColumn(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item { ConnectionCard(owner, viewModel, updateBusy, onRequestBlePermissions, onEnableBluetooth, onOpenLocationSettings, onOpenAppSettings) }
        owner.status?.let { status ->
            item {
                SectionCard(status.companionName) {
                    Text(if (status.mood == "SLEEPING") "Sleeping" else "Awake")
                    status.firmwareVersion?.let { Text("Firmware $it") }
                    status.batteryPercent?.let { Text("Battery $it%") }
                    status.personality?.let { Text("Personality ${it.humanized()}") }
                    status.evolutionStage?.let { Text("Stage ${it.humanized()}") }
                    if (status.packReady) {
                        Text("Pack ${status.packId ?: "ready"}${status.packRevision?.let { " · revision $it" } ?: ""}")
                    }
                    Text("Bond level ${status.bondLevel} · ${status.bondProgressPercent}%")
                    if (status.memoryCount > 0) Text("${status.memoryCount} memories")
                    if (status.listening) Text("Listening")
                    Spacer(Modifier.height(8.dp))
                    Text("Energy ${status.needs.energy}%")
                    Text("Curiosity ${status.needs.curiosity}%")
                    Text("Affection ${status.needs.affection}%")
                }
            }
        } ?: item {
            SectionCard("Your Kitsu") { Text("Pair and connect a nearby Kitsu to see its local state.") }
        }
        if (owner.history.isNotEmpty()) {
            item { Text("Recent mesh activity", style = MaterialTheme.typography.titleMedium) }
            items(owner.history.takeLast(5).reversed()) { entry ->
                SectionCard(entry.kind) { Text(entry.summary) }
            }
        }
    }
}

@Composable
private fun ConnectionCard(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    val selected = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }
    val locationSettingsAction = locationSettingsActionState(
        owner.connection.detail,
        owner.errorCode,
        updateBusy,
    )
    SectionCard("Connection") {
        Text(selected?.displayName ?: "No Kitsu selected", fontWeight = FontWeight.SemiBold)
        Text(connectionText(owner))
        owner.errorCode?.let { Text(it.humanized(), color = MaterialTheme.colorScheme.error) }
        if (updateBusy) Text(UPDATE_LOCKED_COPY)
        Spacer(Modifier.height(8.dp))
        when {
            owner.connection.connected -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = viewModel::refresh,
                    enabled = !updateBusy && !owner.loading,
                    modifier = Modifier.testTag("connection-refresh"),
                ) { Text("Refresh") }
                Button(
                    onClick = viewModel::disconnect,
                    enabled = !updateBusy,
                    modifier = Modifier.testTag("connection-disconnect"),
                ) { Text("Disconnect") }
            }
            owner.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onRequestBlePermissions, enabled = !updateBusy) { Text("Allow Bluetooth") }
                TextButton(onClick = onOpenAppSettings, enabled = !updateBusy) { Text("App settings") }
            }
            owner.connection.detail == "bluetooth_disabled" -> Button(
                onClick = onEnableBluetooth,
                enabled = !updateBusy,
            ) { Text("Turn on Bluetooth") }
            locationSettingsAction.visible -> Button(
                onClick = onOpenLocationSettings,
                enabled = locationSettingsAction.enabled,
                modifier = Modifier.testTag("connection-location-settings"),
            ) { Text("Location settings") }
            else -> Button(
                onClick = { viewModel.connect() },
                enabled = selected != null && !owner.loading && !owner.pairing && !updateBusy,
                modifier = Modifier.testTag("connection-connect"),
            ) { Text(if (owner.loading) "Connecting…" else "Connect") }
        }
    }
}

@Composable
private fun MessagesScreen(owner: OwnerState, viewModel: MainViewModel, updateBusy: Boolean, modifier: Modifier) {
    var route by remember { mutableStateOf(MessageRoute.DIRECT) }
    var target by remember { mutableStateOf("") }
    var body by remember { mutableStateOf("") }
    val validation = MessageComposerPolicy.validationError(route, target, body)
    val recipients = if (route == MessageRoute.DIRECT) {
        MessageComposerPolicy.contactRecipients(owner.peers)
    } else {
        MessageComposerPolicy.channelRecipients(owner.channels)
    }
    LazyColumn(
        modifier.fillMaxSize().imePadding().padding(16.dp).testTag("message-composer"),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item { Text("Messages", style = MaterialTheme.typography.headlineSmall) }
        if (updateBusy) item { Text(UPDATE_LOCKED_COPY) }
        items(owner.messages.reversed()) { message ->
            SectionCard(message.peerId ?: message.channel?.let { "Channel $it" } ?: "Message") { Text(message.text) }
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(enabled = !updateBusy, onClick = {
                    if (route != MessageRoute.DIRECT) target = ""
                    route = MessageRoute.DIRECT
                }) { Text("Direct") }
                OutlinedButton(enabled = !updateBusy, onClick = {
                    if (route != MessageRoute.CHANNEL) target = ""
                    route = MessageRoute.CHANNEL
                }) { Text("Channel") }
            }
        }
        if (recipients.isNotEmpty()) {
            item { Text(if (route == MessageRoute.DIRECT) "Choose a nearby peer" else "Choose a channel") }
            items(recipients) { recipient ->
                OutlinedButton(
                    onClick = { target = recipient.reference },
                    enabled = !updateBusy,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column {
                        Text(recipient.label)
                        if (route == MessageRoute.DIRECT) {
                            Text(MessageComposerPolicy.compactReference(recipient.reference))
                        }
                    }
                }
            }
        }
        if (target.isNotBlank()) item {
            Text(target, maxLines = 1, modifier = Modifier.testTag("message-compact-target"))
        }
        item {
            OutlinedTextField(
                value = target,
                onValueChange = { target = it },
                label = { Text(if (route == MessageRoute.DIRECT) "Peer key" else "Channel 0–3") },
                modifier = Modifier.fillMaxWidth().testTag("message-target"),
                enabled = !updateBusy,
                singleLine = true,
            )
        }
        item {
            OutlinedTextField(
                value = body,
                onValueChange = { body = it },
                label = { Text("Message") },
                modifier = Modifier.fillMaxWidth().testTag("message-body"),
                enabled = !updateBusy,
                minLines = 3,
            )
        }
        item {
            Button(
                onClick = {
                    viewModel.sendMessage(target, body, route) { body = "" }
                },
                enabled = owner.connection.connected && validation == null && !updateBusy,
                modifier = Modifier.testTag("message-send"),
            ) { Text("Send over mesh") }
            validation?.let { Text(it.humanized(), color = MaterialTheme.colorScheme.error) }
        }
    }
}

@Composable
private fun MeshScreen(owner: OwnerState, viewModel: MainViewModel, updateBusy: Boolean, modifier: Modifier) {
    LazyColumn(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (updateBusy) item { Text(UPDATE_LOCKED_COPY) }
        item {
            SectionCard("Local mesh radio") {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Column {
                        Text(if (owner.status?.mesh?.enabled == true) "Enabled" else "Disabled")
                        Text("Works directly on your Kitsu; no internet is used.")
                    }
                    Switch(
                        checked = owner.status?.mesh?.enabled == true,
                        onCheckedChange = viewModel::configureMesh,
                        enabled = owner.connection.connected && !owner.meshConfigurationInFlight && !updateBusy,
                    )
                }
            }
        }
        item { Text("Nearby peers", style = MaterialTheme.typography.titleMedium) }
        if (owner.peers.isEmpty()) item { Text("No peers heard yet.") }
        items(owner.peers) { peer -> SectionCard(peer.name) { Text(peer.route ?: peer.role) } }
        item { Text("Channels", style = MaterialTheme.typography.titleMedium) }
        items(owner.channels) { channel ->
            SectionCard("Channel ${channel.slot}") { Text(channel.name ?: if (channel.configured) "Configured" else "Not configured") }
        }
    }
}

@Composable
private fun CareScreen(owner: OwnerState, viewModel: MainViewModel, updateBusy: Boolean, modifier: Modifier) {
    LazyColumn(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item { Text("Care happens over your authenticated Bluetooth link.") }
        if (updateBusy) item { Text(UPDATE_LOCKED_COPY) }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button({ viewModel.simpleAction(ActionKind.PET) }, enabled = owner.connection.connected && !updateBusy, modifier = Modifier.weight(1f)) { Text("Pet") }
                Button({ viewModel.simpleAction(ActionKind.FEED) }, enabled = owner.connection.connected && !updateBusy, modifier = Modifier.weight(1f)) { Text("Feed") }
            }
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button({ viewModel.simpleAction(ActionKind.PLAY) }, enabled = owner.connection.connected && !updateBusy, modifier = Modifier.weight(1f)) { Text("Play") }
                Button({ viewModel.simpleAction(ActionKind.LISTEN_ONCE) }, enabled = owner.connection.connected && !updateBusy, modifier = Modifier.weight(1f)) { Text("Listen once") }
            }
        }
    }
}

@Composable
private fun SettingsScreen(
    owner: OwnerState,
    firmware: app.kitsu.mobile.FirmwareUpdateUiState,
    viewModel: MainViewModel,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onPairController: (String) -> Unit,
    onFinishPairing: () -> Unit,
    onOpenFirmwarePackage: () -> Unit,
    onOpenAppSettings: () -> Unit,
    updateBusy: Boolean,
    modifier: Modifier,
) {
    var pairingLabel by remember { mutableStateOf("My phone") }
    var forgetAddress by remember { mutableStateOf<String?>(null) }
    LazyColumn(modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item { Text("Kitsu companion", style = MaterialTheme.typography.headlineSmall) }
        item { Text("Local-first · authenticated Bluetooth only") }
        item { ConnectionCard(owner, viewModel, updateBusy, onRequestBlePermissions, onEnableBluetooth, onOpenLocationSettings, onOpenAppSettings) }
        item { Text("Saved Kitsu (${owner.savedKitsu.size}/$MAX_SAVED_KITSU)", style = MaterialTheme.typography.titleMedium) }
        if (owner.savedKitsu.isEmpty()) item { Text("Pair your first Kitsu. Pairing adds it and makes it active.") }
        items(owner.savedKitsu, key = { it.deviceAddress }) { device ->
            val selected = device.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
            val connected = selected && owner.connection.connected
            SectionCard(device.displayName) {
                Text(if (selected) "Selected" else "Saved")
                Text(device.deviceAddress)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (!selected) OutlinedButton(
                        onClick = { viewModel.selectDevice(device.deviceAddress) },
                        enabled = !updateBusy,
                    ) { Text("Select") }
                    if (connected) OutlinedButton(
                        onClick = viewModel::disconnect,
                        enabled = !updateBusy,
                    ) { Text("Disconnect") }
                    else Button(
                        onClick = { viewModel.connect(device.deviceAddress) },
                        enabled = !updateBusy,
                    ) { Text("Connect") }
                }
                TextButton(
                    onClick = { forgetAddress = device.deviceAddress },
                    enabled = !updateBusy,
                ) {
                    Text(if (owner.pendingForgetAddress.equals(device.deviceAddress, true)) "Finish forgetting" else "Forget authorization")
                }
            }
        }
        item {
            SectionCard("Pair another Kitsu") {
                owner.pendingPairing?.let { pending ->
                    Text("Pairing with ${pending.displayName} may already be committed. Keep it nearby and finish pairing.")
                    Button(
                        onClick = onFinishPairing,
                        enabled = !owner.pairing && !updateBusy,
                    ) { Text(if (owner.pairing) "Finishing…" else "Finish pairing") }
                } ?: run {
                    OutlinedTextField(
                        pairingLabel,
                        { pairingLabel = it },
                        label = { Text("Phone label") },
                        singleLine = true,
                        enabled = !updateBusy,
                    )
                    Spacer(Modifier.height(8.dp))
                    Button(
                        onClick = { onPairController(pairingLabel) },
                        enabled = owner.savedKitsu.size < MAX_SAVED_KITSU && !owner.pairing && !updateBusy,
                    ) { Text(if (owner.pairing) "Pairing…" else "Pair this phone") }
                }
                owner.pairingProgress?.let { Text(it.detail.humanized()) }
                if (owner.pairing) TextButton(
                    onClick = viewModel::cancelPairing,
                    enabled = !updateBusy,
                ) { Text("Cancel pairing") }
            }
        }
        item {
            SectionCard("Offline firmware update") {
                Text("Download a signed .kitsu-fw package in your browser, then choose it here. Verification and installation use no internet.")
                Text("During installation, keep Kitsu powered, nearby, and this screen open.")
                Spacer(Modifier.height(8.dp))
                if (updateBusy) Text(UPDATE_LOCKED_COPY)
                OutlinedButton(
                    onClick = onOpenFirmwarePackage,
                    enabled = !updateBusy,
                ) { Text("Choose .kitsu-fw") }
                firmware.progress.firmwareVersion?.let { Text("Signed firmware $it") }
                firmware.importedReleaseId?.let { Text("Release $it") }
                when (firmware.progress.stage) {
                    FirmwareInstallStage.TRANSFERRING -> {
                        LinearProgressIndicator(
                            progress = { firmware.progress.percent / 100f },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Text("${firmware.progress.percent}%")
                    }
                    FirmwareInstallStage.PREPARING, FirmwareInstallStage.VERIFYING,
                    FirmwareInstallStage.READY_TO_REBOOT, FirmwareInstallStage.REBOOTING ->
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    else -> Unit
                }
                firmware.progress.errorCode?.let { Text(it.humanized(), color = MaterialTheme.colorScheme.error) }
                if (firmware.progress.stage == FirmwareInstallStage.IMPORTED || firmware.progress.stage == FirmwareInstallStage.FAILED) {
                    Button(
                        onClick = viewModel::installImportedFirmware,
                        enabled = firmware.updateId != null && owner.connection.connected,
                    ) { Text("Install on selected Kitsu") }
                }
                if (firmware.progress.stage == FirmwareInstallStage.FAILED && firmware.updateId != null) {
                    OutlinedButton(
                        onClick = viewModel::resetInterruptedFirmwareUpdate,
                        enabled = owner.connection.connected,
                    ) { Text("Reset interrupted update") }
                }
                if (firmware.updateId != null && firmware.progress.stage in setOf(
                        FirmwareInstallStage.TRANSFERRING,
                        FirmwareInstallStage.VERIFYING,
                        FirmwareInstallStage.READY_TO_REBOOT,
                    )
                ) TextButton(onClick = viewModel::cancelFirmwareUpdate) { Text("Cancel update") }
                if (firmware.progress.stage == FirmwareInstallStage.COMPLETE) Text("Update confirmed.")
            }
        }
    }

    forgetAddress?.let { address ->
        val name = owner.savedKitsu.firstOrNull { it.deviceAddress.equals(address, true) }?.displayName ?: "Kitsu"
        AlertDialog(
            onDismissRequest = { forgetAddress = null },
            title = { Text("Forget $name authorization?") },
            text = { Text("Kitsu will revoke this phone's controller root. Packs and other phone authorizations stay intact. Android may still show the Bluetooth bond in system settings.") },
            confirmButton = {
                Button(onClick = {
                    forgetAddress = null
                    viewModel.forgetController(address)
                }, enabled = !updateBusy) { Text("Forget authorization") }
            },
            dismissButton = { TextButton(onClick = { forgetAddress = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            content()
        }
    }
}

private fun connectionText(owner: OwnerState): String = when {
    owner.connection.connected -> "Connected directly over authenticated Bluetooth"
    owner.loading || owner.connection.mode == ConnectionMode.CONNECTING -> "Connecting over Bluetooth…"
    owner.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> "Bluetooth permission is required"
    owner.connection.detail == "bluetooth_disabled" -> "Bluetooth is off"
    owner.connection.detail == "user_disconnected" -> "Disconnected by you"
    owner.activeDeviceAddress == null -> "No saved Kitsu"
    else -> owner.connection.detail.humanized()
}

internal data class LocationSettingsActionState(
    val visible: Boolean,
    val enabled: Boolean,
)

internal fun locationSettingsActionState(
    connectionDetail: String,
    errorCode: String?,
    updateBusy: Boolean,
): LocationSettingsActionState {
    val visible = connectionDetail in LOCATION_SETTINGS_ERRORS ||
        (errorCode != null && errorCode in LOCATION_SETTINGS_ERRORS)
    return LocationSettingsActionState(visible = visible, enabled = visible && !updateBusy)
}

private val LOCATION_SETTINGS_ERRORS = setOf(
    "location_services_disabled",
    "location_services_unavailable",
)

private const val UPDATE_LOCKED_COPY =
    "Firmware update in progress. Other controls are locked until it finishes or is safely cancelled."

private fun String.humanized(): String = replace('_', ' ').replaceFirstChar { it.uppercase() }
