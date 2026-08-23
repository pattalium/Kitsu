package ptl.kitsu.app.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BluetoothConnected
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.Devices
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.SystemUpdateAlt
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.BuildConfig
import ptl.kitsu.app.FirmwareUpdateUiState
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.security.MAX_SAVED_KITSU
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.update.FirmwareInstallStage

@Composable
internal fun KitsuSettingsScreen(
    owner: OwnerState,
    firmware: FirmwareUpdateUiState,
    viewModel: MainViewModel,
    themePreference: KitsuThemePreference,
    onThemePreferenceChange: (KitsuThemePreference) -> Unit,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onPairController: (String) -> Unit,
    onRetryPairingBlePermissions: () -> Unit,
    onFinishPairing: () -> Unit,
    onOpenFirmwarePackage: () -> Unit,
    onOpenAppSettings: () -> Unit,
    acceptedPolicyVersion: Int,
    blockedPeerIds: Set<String>,
    onAcceptPolicy: () -> Unit,
    onUnblockPeer: (String) -> Unit,
    updateBusy: Boolean,
    modifier: Modifier = Modifier,
) {
    var pairingLabel by rememberSaveable { mutableStateOf("My phone") }
    var forgetAddress by rememberSaveable { mutableStateOf<String?>(null) }
    var showTerms by rememberSaveable { mutableStateOf(false) }
    var showPrivacy by rememberSaveable { mutableStateOf(false) }
    var pendingUnblockPeerId by rememberSaveable { mutableStateOf<String?>(null) }

    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-settings"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Settings",
                supporting = "Your devices, appearance and signed offline updates.",
            )
        }

        item {
            SettingsConnectionCard(
                owner = owner,
                viewModel = viewModel,
                updateBusy = updateBusy,
                onRequestBlePermissions = onRequestBlePermissions,
                onEnableBluetooth = onEnableBluetooth,
                onOpenLocationSettings = onOpenLocationSettings,
                onOpenAppSettings = onOpenAppSettings,
            )
        }

        item {
            SectionHeading(
                title = "Your devices",
                supporting = "Up to $MAX_SAVED_KITSU controller authorizations stay encrypted on this phone.",
            )
        }
        if (owner.savedKitsu.isEmpty()) {
            item {
                StatePanel(
                    title = "No Kitsu paired",
                    message = "Keep one nearby and powered, then pair this phone below.",
                    testTag = "saved-kitsu-empty",
                )
            }
        } else {
            items(owner.savedKitsu, key = { it.deviceAddress }) { device ->
                val selected = device.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
                val connected = selected && owner.connection.connected
                KitsuCard(modifier = Modifier.testTag("saved-kitsu-card")) {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(
                            if (connected) Icons.Default.BluetoothConnected else Icons.Default.Devices,
                            contentDescription = null,
                            tint = if (connected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary,
                        )
                        Column(Modifier.weight(1f)) {
                            Text(
                                device.displayName,
                                style = MaterialTheme.typography.titleMedium,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            Text(
                                device.deviceAddress,
                                style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        StatusPill(
                            when {
                                connected -> "Connected"
                                selected -> "Selected"
                                else -> "Saved"
                            },
                            when {
                                connected -> StatusTone.POSITIVE
                                selected -> StatusTone.ACTIVE
                                else -> StatusTone.NEUTRAL
                            },
                        )
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (!selected) {
                            OutlinedButton(
                                onClick = { viewModel.selectDevice(device.deviceAddress) },
                                enabled = !updateBusy,
                            ) { Text("Select") }
                        }
                        if (connected) {
                            OutlinedButton(
                                onClick = viewModel::disconnect,
                                enabled = !updateBusy,
                            ) { Text("Disconnect") }
                        } else {
                            Button(
                                onClick = { viewModel.connect(device.deviceAddress) },
                                enabled = !updateBusy && !owner.loading && !owner.pairing,
                            ) { Text("Connect") }
                        }
                    }
                    TextButton(
                        onClick = { forgetAddress = device.deviceAddress },
                        enabled = !updateBusy,
                    ) {
                        Text(
                            if (owner.pendingForgetAddress.equals(device.deviceAddress, ignoreCase = true)) {
                                "Finish forgetting authorization"
                            } else {
                                "Forget authorization"
                            },
                        )
                    }
                }
            }
        }

        item {
            PairingCard(
                owner = owner,
                pairingLabel = pairingLabel,
                onPairingLabelChange = { pairingLabel = it },
                onPairController = onPairController,
                onRetryBlePermissions = onRetryPairingBlePermissions,
                onOpenAppSettings = onOpenAppSettings,
                onFinishPairing = onFinishPairing,
                onCancelPairing = viewModel::cancelPairing,
                updateBusy = updateBusy,
            )
        }

        item {
            AppearanceCard(
                themePreference = themePreference,
                onThemePreferenceChange = onThemePreferenceChange,
                enabled = !updateBusy,
            )
        }

        item {
            FirmwareCard(
                owner = owner,
                firmware = firmware,
                viewModel = viewModel,
                onOpenFirmwarePackage = onOpenFirmwarePackage,
                updateBusy = updateBusy,
            )
        }

        item {
            KitsuDetailsCard(owner)
        }

        item {
            KitsuCard(title = "Privacy & terms", modifier = Modifier.testTag("settings-privacy-terms")) {
                StatusPill(
                    if (acceptedPolicyVersion == MeshUserPolicy.VERSION) {
                        "Messaging policy accepted"
                    } else {
                        "Acceptance required to send"
                    },
                    if (acceptedPolicyVersion == MeshUserPolicy.VERSION) StatusTone.POSITIVE else StatusTone.ACTIVE,
                )
                Text(
                    "Policy ${MeshUserPolicy.VERSION_LABEL}. Messaging remains gated whenever the policy version changes.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { showTerms = true }, modifier = Modifier.testTag("open-terms")) {
                        Text("Terms & user policy")
                    }
                    TextButton(onClick = { showPrivacy = true }, modifier = Modifier.testTag("open-privacy")) {
                        Text("Privacy")
                    }
                }
            }
        }

        item {
            KitsuCard(title = "Blocked peers", modifier = Modifier.testTag("blocked-peers")) {
                Text(
                    "Blocking hides a peer's messages and recipient suggestion on this phone. Kitsu firmware may still receive that peer's mesh traffic.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (blockedPeerIds.isEmpty()) {
                    Text("No peers blocked on this phone.")
                } else {
                    blockedPeerIds.sorted().forEach { peerId ->
                        Row(
                            Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                MessageComposerPolicy.compactReference(peerId),
                                style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
                                modifier = Modifier.weight(1f),
                            )
                            TextButton(
                                onClick = { pendingUnblockPeerId = peerId },
                                enabled = !updateBusy,
                            ) { Text("Unblock") }
                        }
                    }
                }
            }
        }

        item {
            KitsuCard(title = "App identity", modifier = Modifier.testTag("settings-separate-install")) {
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
                    Icon(Icons.Default.Info, contentDescription = null, tint = MaterialTheme.colorScheme.secondary)
                    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text("${BuildConfig.APPLICATION_ID} · ${BuildConfig.VERSION_NAME}")
                        Text(
                            "This Play app identity is a separate install from the retired pre-Play build. It is not an in-place upgrade and does not inherit that app's saved authorizations; pair your Kitsu again.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }

    forgetAddress?.let { address ->
        val name = owner.savedKitsu.firstOrNull {
            it.deviceAddress.equals(address, ignoreCase = true)
        }?.displayName ?: "Kitsu"
        AlertDialog(
            onDismissRequest = { forgetAddress = null },
            icon = { Icon(Icons.Default.Security, contentDescription = null) },
            title = { Text("Forget $name authorization?") },
            text = {
                Text(
                    "Kitsu will revoke this phone's controller root. Packs and other phone authorizations stay intact. Android may still show the Bluetooth bond in system settings.",
                )
            },
            confirmButton = {
                Button(
                    onClick = {
                        forgetAddress = null
                        viewModel.forgetController(address)
                    },
                    enabled = !updateBusy,
                ) { Text("Forget authorization") }
            },
            dismissButton = {
                TextButton(onClick = { forgetAddress = null }) { Text("Cancel") }
            },
        )
    }

    if (showTerms) {
        MeshPolicyDialog(
            canAccept = !updateBusy,
            onAccept = {
                onAcceptPolicy()
                showTerms = false
            },
            onDismiss = { showTerms = false },
        )
    }

    if (showPrivacy) {
        AlertDialog(
            onDismissRequest = { showPrivacy = false },
            title = { Text("Privacy") },
            text = { Text(MeshUserPolicy.PRIVACY) },
            confirmButton = {
                Button(onClick = { showPrivacy = false }) { Text("Done") }
            },
        )
    }

    pendingUnblockPeerId?.let { peerId ->
        AlertDialog(
            onDismissRequest = { pendingUnblockPeerId = null },
            title = { Text("Unblock this peer?") },
            text = { Text("Its messages and recipient suggestion will be visible again on this phone.") },
            confirmButton = {
                Button(onClick = {
                    onUnblockPeer(peerId)
                    pendingUnblockPeerId = null
                }) { Text("Unblock") }
            },
            dismissButton = {
                TextButton(onClick = { pendingUnblockPeerId = null }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun SettingsConnectionCard(
    owner: OwnerState,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenAppSettings: () -> Unit,
) {
    val presentation = connectionPresentation(owner)
    val selected = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }
    val locationAction = locationSettingsActionState(owner.connection.detail, owner.errorCode, updateBusy)
    KitsuCard(title = "Bluetooth connection") {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(presentation.icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f)) {
                Text(selected?.displayName ?: "No Kitsu selected", style = MaterialTheme.typography.titleMedium)
                Text(
                    presentation.detail,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            StatusPill(presentation.label, presentation.tone)
        }
        if (updateBusy) Text(UPDATE_LOCKED_COPY, style = MaterialTheme.typography.bodyMedium)
        when {
            owner.connection.connected -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = viewModel::refresh,
                    enabled = !owner.loading && !updateBusy,
                ) { Text("Refresh") }
                Button(
                    onClick = viewModel::disconnect,
                    enabled = !updateBusy,
                ) { Text("Disconnect") }
            }
            owner.connection.mode == ConnectionMode.PERMISSION_REQUIRED ||
                owner.errorCode == "bluetooth_permission_required" -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    BLE_PERMISSION_COPY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onRequestBlePermissions, enabled = !updateBusy) { Text("Allow Bluetooth") }
                    TextButton(onClick = onOpenAppSettings, enabled = !updateBusy) { Text("App settings") }
                }
            }
            owner.connection.detail == "bluetooth_disabled" || owner.errorCode == "bluetooth_disabled" ->
                Button(onClick = onEnableBluetooth, enabled = !updateBusy) { Text("Turn on Bluetooth") }
            locationAction.visible -> Button(
                onClick = onOpenLocationSettings,
                enabled = locationAction.enabled,
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
private fun PairingCard(
    owner: OwnerState,
    pairingLabel: String,
    onPairingLabelChange: (String) -> Unit,
    onPairController: (String) -> Unit,
    onRetryBlePermissions: () -> Unit,
    onOpenAppSettings: () -> Unit,
    onFinishPairing: () -> Unit,
    onCancelPairing: () -> Unit,
    updateBusy: Boolean,
) {
    KitsuCard(title = "Pair another Kitsu", modifier = Modifier.testTag("pairing-card")) {
        if (owner.errorCode == "controller_full") {
            Text(
                owner.errorCode.humanized(),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.testTag("controller-full-recovery"),
            )
        }
        when {
            owner.errorCode == "pairing_bluetooth_permission_required" -> {
                Text(
                    BLE_PERMISSION_COPY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onRetryBlePermissions,
                        enabled = !updateBusy,
                        modifier = Modifier.testTag("pairing-allow-bluetooth"),
                    ) { Text("Allow Bluetooth") }
                    TextButton(onClick = onOpenAppSettings, enabled = !updateBusy) {
                        Text("App settings")
                    }
                }
            }
            owner.pendingPairing != null -> {
                StatusPill("Action required", StatusTone.ACTIVE)
                Text(
                    "Pairing with ${owner.pendingPairing.displayName} may already be committed on the device. Keep it nearby and finish the recovery step.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(
                    onClick = onFinishPairing,
                    enabled = !owner.pairing && !updateBusy,
                    modifier = Modifier.testTag("pairing-finish"),
                ) { Text(if (owner.pairing) "Finishing…" else "Finish pairing") }
            }
            owner.pairing -> {
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator()
                    Column {
                        Text("Pairing nearby Kitsu", style = MaterialTheme.typography.titleMedium)
                        Text(
                            owner.pairingProgress?.detail?.humanized() ?: "Waiting for the secure handshake",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                TextButton(onClick = onCancelPairing, enabled = !updateBusy) { Text("Cancel pairing") }
            }
            else -> {
                Text(
                    "Hold the device's pairing control, keep it close, then authorize this phone over Bluetooth.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = pairingLabel,
                    onValueChange = onPairingLabelChange,
                    label = { Text("Phone label") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    enabled = !updateBusy,
                )
                Button(
                    onClick = { onPairController(pairingLabel) },
                    enabled = owner.savedKitsu.size < MAX_SAVED_KITSU && !updateBusy,
                    modifier = Modifier.fillMaxWidth().testTag("pairing-start"),
                ) { Text("Pair this phone") }
                if (owner.savedKitsu.size >= MAX_SAVED_KITSU) {
                    Text(
                        "The saved-device limit is full. Forget one authorization before pairing another.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        }
    }
}

@Composable
private fun AppearanceCard(
    themePreference: KitsuThemePreference,
    onThemePreferenceChange: (KitsuThemePreference) -> Unit,
    enabled: Boolean,
) {
    KitsuCard(title = "Appearance") {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.DarkMode, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Text(
                "Dark is the Kitsu default. Following the phone theme is optional.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
        }
        ThemeChoiceRow(
            label = "Always dark",
            supporting = "Near-black canvas and warm ivory text",
            selected = themePreference == KitsuThemePreference.DARK,
            enabled = enabled,
            modifier = Modifier.testTag("theme-dark"),
        ) { onThemePreferenceChange(KitsuThemePreference.DARK) }
        ThemeChoiceRow(
            label = "Follow phone theme",
            supporting = "Uses light colors only when Android is in light mode",
            selected = themePreference == KitsuThemePreference.SYSTEM,
            enabled = enabled,
            modifier = Modifier.testTag("theme-system"),
        ) { onThemePreferenceChange(KitsuThemePreference.SYSTEM) }
    }
}

@Composable
private fun ThemeChoiceRow(
    label: String,
    supporting: String,
    selected: Boolean,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Row(
        modifier = modifier.fillMaxWidth().clickable(enabled = enabled, onClick = onClick).padding(vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = onClick, enabled = enabled)
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.titleMedium)
            Text(
                supporting,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun FirmwareCard(
    owner: OwnerState,
    firmware: FirmwareUpdateUiState,
    viewModel: MainViewModel,
    onOpenFirmwarePackage: () -> Unit,
    updateBusy: Boolean,
) {
    KitsuCard(title = "Offline firmware update", modifier = Modifier.testTag("firmware-card")) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
            Icon(Icons.Default.SystemUpdateAlt, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Text(
                "Choose a signed .kitsu-fw package already on this phone. Verification and installation use no internet.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
        }
        OutlinedButton(
            onClick = onOpenFirmwarePackage,
            enabled = !updateBusy,
            modifier = Modifier.fillMaxWidth().testTag("firmware-choose"),
        ) { Text("Choose signed .kitsu-fw") }
        firmware.progress.firmwareVersion?.let { Text("Signed firmware $it") }
        firmware.importedReleaseId?.let { Text("Release $it", style = MaterialTheme.typography.bodyMedium) }
        when (firmware.progress.stage) {
            FirmwareInstallStage.IDLE -> Text(
                "No package selected.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            FirmwareInstallStage.IMPORTED -> StatusPill("Verified and ready", StatusTone.POSITIVE)
            FirmwareInstallStage.PREPARING,
            FirmwareInstallStage.VERIFYING,
            FirmwareInstallStage.READY_TO_REBOOT,
            FirmwareInstallStage.REBOOTING -> LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            FirmwareInstallStage.TRANSFERRING -> {
                LinearProgressIndicator(
                    progress = { firmware.progress.percent / 100f },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text("${firmware.progress.percent}% transferred")
            }
            FirmwareInstallStage.COMPLETE -> StatusPill("Update confirmed", StatusTone.POSITIVE)
            FirmwareInstallStage.FAILED -> StatusPill("Update needs attention", StatusTone.NEGATIVE)
        }
        firmware.progress.errorCode?.let {
            Text(it.humanized(), color = MaterialTheme.colorScheme.error)
        }
        if (firmware.progress.stage in setOf(FirmwareInstallStage.IMPORTED, FirmwareInstallStage.FAILED)) {
            Button(
                onClick = viewModel::installImportedFirmware,
                enabled = firmware.updateId != null && owner.connection.connected && !updateBusy,
                modifier = Modifier.fillMaxWidth().testTag("firmware-install"),
            ) { Text("Install on selected Kitsu") }
        }
        if (firmware.progress.stage == FirmwareInstallStage.FAILED && firmware.updateId != null) {
            OutlinedButton(
                onClick = viewModel::resetInterruptedFirmwareUpdate,
                enabled = owner.connection.connected && !updateBusy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Reset interrupted update") }
        }
        if (!owner.connection.connected && firmware.progress.stage in setOf(
                FirmwareInstallStage.IMPORTED,
                FirmwareInstallStage.FAILED,
            )
        ) {
            Text(
                "Connect the selected Kitsu before installing.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun KitsuDetailsCard(owner: OwnerState) {
    val status = owner.status
    KitsuCard(title = "Device details") {
        if (status == null) {
            Text(
                "Connect your Kitsu to read firmware and pack details.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            DetailRow("Firmware", status.firmwareVersion ?: "Not reported")
            DetailRow("Device", status.deviceId)
            DetailRow(
                "Pack",
                if (status.packReady) {
                    "${status.packId ?: "Ready"}${status.packRevision?.let { " · revision $it" } ?: ""}"
                } else {
                    "Not ready"
                },
            )
            status.personality?.let { DetailRow("Personality", it.humanized()) }
            DetailRow("Memories", status.memoryCount.toString())
        }
    }
}

@Composable
private fun DetailRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(
            value,
            modifier = Modifier.weight(1f).padding(start = 16.dp),
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
