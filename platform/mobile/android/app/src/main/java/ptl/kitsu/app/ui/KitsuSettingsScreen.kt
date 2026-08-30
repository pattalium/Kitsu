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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BluetoothConnected
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.Devices
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Key
import androidx.compose.material.icons.filled.OpenInBrowser
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.SystemUpdateAlt
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Switch
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.BuildConfig
import ptl.kitsu.app.EncounterUnlockUiState
import ptl.kitsu.app.FirmwareUpdateUiState
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.automation.KitsuAutomationAction
import ptl.kitsu.app.automation.KitsuAutomationPolicy
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterUnlockCode
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.notifications.KitsuNotificationSettings
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.pairing.ControllerPairingFlow
import ptl.kitsu.app.security.ControllerAccessPolicy
import ptl.kitsu.app.security.ControllerRole
import ptl.kitsu.app.security.MAX_SAVED_KITSU
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.update.FirmwareInstallStage
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle

@Composable
internal fun KitsuSettingsScreen(
    owner: OwnerState,
    firmware: FirmwareUpdateUiState,
    encounterUnlocks: EncounterUnlockUiState,
    viewModel: MainViewModel,
    themePreference: KitsuThemePreference,
    onThemePreferenceChange: (KitsuThemePreference) -> Unit,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onPairController: (String) -> Unit,
    onPairCaretakerController: (String) -> Unit,
    onRetryPairingBlePermissions: () -> Unit,
    onFinishPairing: () -> Unit,
    onRepairBluetoothPairing: () -> Unit,
    onOpenBluetoothSettingsForRepair: () -> Unit,
    onOpenFirmwarePackage: () -> Unit,
    onOpenAppSettings: () -> Unit,
    onOpenSupportPage: () -> Unit,
    onOpenUnlockPage: (String) -> Unit,
    onCopyUnlockCode: (String) -> Unit,
    onRequestNotificationPermission: (continuity: Boolean) -> Unit,
    onCopyAutomationToken: (String) -> Unit,
    acceptedPolicyVersion: Int,
    blockedPeerIds: Set<String>,
    onAcceptPolicy: () -> Unit,
    onUnblockPeer: (String) -> Unit,
    updateBusy: Boolean,
    modifier: Modifier = Modifier,
) {
    val notificationSettings by viewModel.notificationSettings.collectAsStateWithLifecycle()
    val automationCapabilityToken by viewModel.automationCapabilityToken.collectAsStateWithLifecycle()
    var pairingLabel by rememberSaveable { mutableStateOf("My phone") }
    var pairingFlow by rememberSaveable { mutableStateOf(ControllerPairingFlow.OWNER) }
    var forgetAddress by rememberSaveable { mutableStateOf<String?>(null) }
    var showTerms by rememberSaveable { mutableStateOf(false) }
    var showPrivacy by rememberSaveable { mutableStateOf(false) }
    var pendingUnblockPeerId by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingDeleteUnlockDeviceId by rememberSaveable { mutableStateOf<String?>(null) }
    val activeRole = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }?.role ?: ControllerRole.OWNER
    val ownerSettingsAvailable = ControllerAccessPolicy.isOwner(activeRole)

    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-settings"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Settings",
                supporting = if (ownerSettingsAvailable) {
                    "Your devices, appearance and signed offline updates."
                } else {
                    "Your devices, caretaker access and app preferences."
                },
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
                onRepairBluetoothPairing = onRepairBluetoothPairing,
                onOpenBluetoothSettingsForRepair = onOpenBluetoothSettingsForRepair,
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
                val canRemoveRevokedAuthorization =
                    ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                        role = device.role,
                        deviceAddress = device.deviceAddress,
                        activeDeviceAddress = owner.activeDeviceAddress,
                        errorCode = owner.errorCode,
                    )
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
                            Text(
                                "${ControllerPairingPresentation.savedRoleLabel(device.role)} authorization",
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.testTag("saved-kitsu-role"),
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
                    if (ControllerAccessPolicy.allowsOperation(device.role, "controller.forget")) {
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
                    } else if (canRemoveRevokedAuthorization) {
                        TextButton(
                            onClick = { forgetAddress = device.deviceAddress },
                            enabled = !updateBusy,
                            modifier = Modifier.testTag("remove-revoked-authorization"),
                        ) {
                            Text("Remove revoked authorization from this phone")
                        }
                    } else {
                        Text(
                            "An owner can revoke this caretaker authorization on Kitsu.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.testTag("caretaker-revoke-guidance"),
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
                pairingFlow = pairingFlow,
                onPairingFlowChange = { pairingFlow = it },
                onPairController = onPairController,
                onPairCaretakerController = onPairCaretakerController,
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
            NotificationSettingsCard(
                owner = owner,
                settings = notificationSettings,
                directMessagesVisible = ownerSettingsAvailable,
                enabled = !updateBusy,
                onEnableAlerts = { onRequestNotificationPermission(false) },
                onDisableAlerts = viewModel::disableNotificationAlerts,
                onDirectMessagesChanged = viewModel::setDirectMessageNotifications,
                onPetUpdatesChanged = viewModel::setPetUpdateNotifications,
                onEnableContinuity = { onRequestNotificationPermission(true) },
                onDisableContinuity = { viewModel.disableConnectionContinuity(showNotice = true) },
            )
        }

        item {
            LocalAutomationCard(
                capabilityToken = automationCapabilityToken,
                enabled = !updateBusy,
                onEnable = viewModel::enableLocalAutomation,
                onDisable = viewModel::disableLocalAutomation,
                onCopyToken = onCopyAutomationToken,
            )
        }

        if (ownerSettingsAvailable) {
            item {
                FirmwareCard(
                    owner = owner,
                    firmware = firmware,
                    viewModel = viewModel,
                    onOpenFirmwarePackage = onOpenFirmwarePackage,
                    updateBusy = updateBusy,
                )
            }
        }

        item {
            KitsuDetailsCard(owner)
        }

        if (ownerSettingsAvailable) {
            item {
                EncounterUnlocksCard(
                    state = encounterUnlocks,
                    connected = owner.connection.connected,
                    enabled = !updateBusy,
                    onSync = viewModel::syncEncounterCodes,
                    onOpenUnlockPage = onOpenUnlockPage,
                    onCopyUnlockCode = onCopyUnlockCode,
                    onDeleteDevice = { pendingDeleteUnlockDeviceId = it },
                )
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
        }

        item {
            KitsuCard(title = "Support Kitsu", modifier = Modifier.testTag("settings-support-kofi")) {
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
                    Icon(
                        Icons.Default.Favorite,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.primary,
                    )
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(
                            "If Kitsu is useful to you, you can leave a voluntary tip on Ko-fi.",
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        Text(
                            "This opens your web browser. Supporting Kitsu unlocks no app feature, content, badge or other benefit, and Kitsu itself keeps no internet permission.",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        OutlinedButton(
                            onClick = onOpenSupportPage,
                            modifier = Modifier.testTag("open-support-kofi"),
                        ) {
                            Text("Support Kitsu on Ko-fi")
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
        val device = owner.savedKitsu.firstOrNull {
            it.deviceAddress.equals(address, ignoreCase = true)
        }
        val canRevokeOnKitsu = device?.let {
            ControllerAccessPolicy.allowsOperation(it.role, "controller.forget")
        } == true
        val canRemoveRevokedAuthorization = device?.let {
            ControllerPairingPresentation.canRemoveRevokedCaretakerAuthorization(
                role = it.role,
                deviceAddress = it.deviceAddress,
                activeDeviceAddress = owner.activeDeviceAddress,
                errorCode = owner.errorCode,
            )
        } == true
        if (device != null && (canRevokeOnKitsu || canRemoveRevokedAuthorization)) {
            AlertDialog(
                onDismissRequest = { forgetAddress = null },
                icon = { Icon(Icons.Default.Security, contentDescription = null) },
                title = {
                    Text(
                        if (canRevokeOnKitsu) {
                            "Forget ${device.displayName} authorization?"
                        } else {
                            "Remove ${device.displayName} from this phone?"
                        },
                    )
                },
                text = {
                    Text(
                        if (canRevokeOnKitsu) {
                            "Kitsu will revoke this phone's controller root. Packs, other phone authorizations and saved encounter unlocks stay intact. Delete saved unlocks separately if you want them removed from this phone. Android may still show the Bluetooth bond in system settings."
                        } else {
                            "Kitsu has already rejected this revoked caretaker authorization. This removes only its encrypted credential from this phone; Kitsu and other phones stay unchanged."
                        },
                    )
                },
                confirmButton = {
                    Button(
                        onClick = {
                            forgetAddress = null
                            viewModel.forgetController(address)
                        },
                        enabled = !updateBusy,
                    ) {
                        Text(if (canRevokeOnKitsu) "Forget authorization" else "Remove from phone")
                    }
                },
                dismissButton = {
                    TextButton(onClick = { forgetAddress = null }) { Text("Cancel") }
                },
            )
        }
    }

    if (showTerms && ownerSettingsAvailable) {
        MeshPolicyDialog(
            canAccept = !updateBusy,
            onAccept = {
                onAcceptPolicy()
                showTerms = false
            },
            onDismiss = { showTerms = false },
        )
    }

    if (showPrivacy && ownerSettingsAvailable) {
        AlertDialog(
            onDismissRequest = { showPrivacy = false },
            title = { Text("Privacy") },
            text = { Text(MeshUserPolicy.PRIVACY) },
            confirmButton = {
                Button(onClick = { showPrivacy = false }) { Text("Done") }
            },
        )
    }

    pendingUnblockPeerId?.takeIf { ownerSettingsAvailable }?.let { peerId ->
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

    pendingDeleteUnlockDeviceId?.takeIf { ownerSettingsAvailable }?.let { deviceId ->
        AlertDialog(
            onDismissRequest = { pendingDeleteUnlockDeviceId = null },
            icon = { Icon(Icons.Default.DeleteSweep, contentDescription = null) },
            title = { Text("Delete saved unlocks for $deviceId?") },
            text = {
                Text(
                    "This removes only this Kitsu's encrypted code copies from this phone. It does not revoke codes stored on the Kitsu.",
                )
            },
            confirmButton = {
                Button(
                    onClick = {
                        pendingDeleteUnlockDeviceId = null
                        viewModel.deleteEncounterCodesForDevice(deviceId)
                    },
                    enabled = !updateBusy,
                ) { Text("Delete saved unlocks") }
            },
            dismissButton = {
                TextButton(onClick = { pendingDeleteUnlockDeviceId = null }) { Text("Cancel") }
            },
        )
    }
}

@Composable
private fun EncounterUnlocksCard(
    state: EncounterUnlockUiState,
    connected: Boolean,
    enabled: Boolean,
    onSync: () -> Unit,
    onOpenUnlockPage: (String) -> Unit,
    onCopyUnlockCode: (String) -> Unit,
    onDeleteDevice: (String) -> Unit,
) {
    var revealedCodeKey by rememberSaveable { mutableStateOf<String?>(null) }
    val groups = state.records.groupBy(EncounterUnlockCode::deviceId)
    KitsuCard(
        title = "Saved creature unlocks",
        modifier = Modifier.testTag("encounter-unlocks"),
    ) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Icon(
                Icons.Default.Key,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    "Codes from every Kitsu stay encrypted in this phone's separate vault.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    "Switching devices or forgetting a controller authorization does not delete them.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        OutlinedButton(
            onClick = onSync,
            enabled = connected && enabled && !state.syncing,
            modifier = Modifier.fillMaxWidth().testTag("encounter-unlocks-sync"),
        ) {
            Text(if (state.syncing) "Syncing from Kitsu..." else "Sync from connected Kitsu")
        }
        if (!connected) {
            Text(
                "Connect a Kitsu to sync. Codes already saved remain available offline.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (state.loading || state.syncing) {
            LinearProgressIndicator(Modifier.fillMaxWidth())
        }
        state.errorCode?.let { error ->
            Text(
                if (error == "firmware_operation_unavailable") {
                    "This firmware does not expose encounter unlocks yet. Saved codes remain intact."
                } else {
                    error.humanized()
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        }
        if (!state.loading && groups.isEmpty()) {
            Text(
                "No creature unlocks are saved on this phone yet.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("encounter-unlocks-empty"),
            )
        }
        groups.forEach { (deviceId, records) ->
            HorizontalDivider()
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("Kitsu $deviceId", style = MaterialTheme.typography.titleMedium)
                    Text(
                        "Hardware-bound unlocks",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                TextButton(
                    onClick = { onDeleteDevice(deviceId) },
                    enabled = enabled,
                    modifier = Modifier.testTag("encounter-unlocks-delete-$deviceId"),
                ) { Text("Delete") }
            }
            records.forEachIndexed { index, record ->
                EncounterUnlockRow(
                    value = record,
                    revealed = revealedCodeKey == record.vaultKey,
                    enabled = enabled,
                    onToggleReveal = {
                        revealedCodeKey = if (revealedCodeKey == record.vaultKey) null else record.vaultKey
                    },
                    onCopy = {
                        onCopyUnlockCode(record.code)
                    },
                    onOpen = { onOpenUnlockPage(record.code) },
                )
                if (index != records.lastIndex) HorizontalDivider()
            }
        }
        if (groups.isNotEmpty()) {
            Text(
                "Open unlock page uses your browser. The Kitsu app itself has no internet permission.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun EncounterUnlockRow(
    value: EncounterUnlockCode,
    revealed: Boolean,
    enabled: Boolean,
    onToggleReveal: () -> Unit,
    onCopy: () -> Unit,
    onOpen: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().padding(vertical = 6.dp).testTag("encounter-unlock-row"),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(encounterCreatureLabel(value), style = MaterialTheme.typography.titleMedium)
                Text(
                    encounterAcquiredLabel(value.acquiredAtEpoch),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            StatusPill(
                label = encounterRarityLabel(value.rarity),
                tone = if (value.rarity == EncounterRarity.MYTHICAL) StatusTone.ACTIVE else StatusTone.NEUTRAL,
            )
        }
        Text(
            if (revealed) value.code else maskEncounterCode(value.code),
            style = MaterialTheme.typography.bodyLarge.copy(fontFamily = FontFamily.Monospace),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.testTag("encounter-unlock-code"),
        )
        if (value.installed || value.redeemed) {
            Text(
                if (value.installed) "Installed" else "Redeemed",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = onToggleReveal, enabled = enabled) {
                Icon(
                    if (revealed) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                    contentDescription = null,
                )
                Spacer(Modifier.width(4.dp))
                Text(if (revealed) "Hide" else "Show")
            }
            IconButton(
                onClick = onCopy,
                enabled = enabled,
                modifier = Modifier.testTag("encounter-unlock-copy"),
            ) {
                Icon(Icons.Default.ContentCopy, contentDescription = "Copy unlock code")
            }
        }
        OutlinedButton(
            onClick = onOpen,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth().testTag("encounter-unlock-open"),
        ) {
            Icon(Icons.Default.OpenInBrowser, contentDescription = null)
            Spacer(Modifier.width(4.dp))
            Text("Open unlock page", maxLines = 1)
        }
    }
}

internal fun maskEncounterCode(code: String): String = when {
    code.length <= 8 -> "•".repeat(code.length)
    else -> code.take(4) + "••••••" + code.takeLast(4)
}

private fun encounterCreatureLabel(value: EncounterUnlockCode): String =
    value.creatureName ?: value.packId?.let { "Creature pack ${it.toString(16).uppercase().padStart(8, '0')}" }
        ?: "Creature unlock"

private fun encounterRarityLabel(value: EncounterRarity): String = when (value) {
    EncounterRarity.COMMON -> "Common"
    EncounterRarity.UNCOMMON -> "Uncommon"
    EncounterRarity.RARE -> "Rare"
    EncounterRarity.VERY_RARE -> "Very rare"
    EncounterRarity.EPIC -> "Epic"
    EncounterRarity.LEGENDARY -> "Legendary"
    EncounterRarity.MYTHICAL -> "Mythical"
}

private fun encounterAcquiredLabel(epochSeconds: Long): String {
    if (epochSeconds <= 0L) return "Acquired time unavailable"
    return runCatching {
        val date = Instant.ofEpochSecond(epochSeconds).atZone(ZoneId.systemDefault()).toLocalDate()
        "Acquired ${DateTimeFormatter.ofLocalizedDate(FormatStyle.MEDIUM).format(date)}"
    }.getOrDefault("Acquired time unavailable")
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
    onRepairBluetoothPairing: () -> Unit,
    onOpenBluetoothSettingsForRepair: () -> Unit,
) {
    val presentation = connectionPresentation(owner)
    val selected = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }
    val locationAction = locationSettingsActionState(owner.connection.detail, owner.errorCode, updateBusy)
    val repairCode = sequenceOf(owner.errorCode, owner.connection.detail)
        .firstOrNull {
            BluetoothPairingRepairPolicy.shouldOfferRepair(it) ||
                it == BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING
        }
        ?: owner.errorCode
        ?: owner.connection.detail
    val repairAvailable = selected != null &&
        BluetoothPairingRepairPolicy.shouldOfferRepair(repairCode)
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
                owner.errorCode == "bluetooth_permission_required" ||
                owner.errorCode == BluetoothPairingRepairPolicy.PERMISSION_REQUIRED -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    BLE_PERMISSION_COPY,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = if (owner.errorCode == BluetoothPairingRepairPolicy.PERMISSION_REQUIRED) {
                            onRepairBluetoothPairing
                        } else onRequestBlePermissions,
                        enabled = !updateBusy,
                    ) { Text("Allow Bluetooth") }
                    TextButton(onClick = onOpenAppSettings, enabled = !updateBusy) { Text("App settings") }
                }
            }
            owner.repairingBluetoothPairing -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CircularProgressIndicator()
                    Column {
                        Text("Repairing Bluetooth pairing", style = MaterialTheme.typography.titleMedium)
                        Text(
                            owner.bluetoothPairingRepairProgress?.detail?.humanized()
                                ?: "Waiting for secure Android pairing",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                Text(
                    "Your saved controller authorization, pet, packs, encounter unlocks, and app data stay unchanged.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                TextButton(
                    onClick = viewModel::cancelBluetoothPairingRepair,
                    enabled = !updateBusy,
                    modifier = Modifier.testTag("bluetooth-repair-cancel"),
                ) { Text("Cancel repair") }
            }
            repairAvailable -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                StatusPill("Repair needed", StatusTone.ACTIVE)
                Text(
                    if (BluetoothPairingRepairPolicy.requiresAndroidForget(repairCode)) {
                        "First, on Kitsu use CONNECT > CONTROLLERS > CLEAR BLE BONDS / CONTROLLERS KEPT and hold PRG for 5 seconds. Then open CONNECT > BLUETOOTH > PAIR PHONE. Open Android Bluetooth settings, Forget this Kitsu, and return here; repair resumes with the same saved controller authorization."
                    } else {
                        "Android and Kitsu no longer agree on the Bluetooth security bond. On Kitsu, use CONNECT > CONTROLLERS > CLEAR BLE BONDS / CONTROLLERS KEPT and hold PRG for 5 seconds, then open CONNECT > BLUETOOTH > PAIR PHONE and repair here. This reuses the existing controller ID and does not consume another controller slot."
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.testTag("bluetooth-repair-guidance"),
                )
                Button(
                    onClick = if (BluetoothPairingRepairPolicy.requiresAndroidForget(repairCode)) {
                        onOpenBluetoothSettingsForRepair
                    } else onRepairBluetoothPairing,
                    enabled = !owner.loading && !owner.pairing && !updateBusy,
                    modifier = Modifier.fillMaxWidth().testTag("bluetooth-repair-start"),
                ) {
                    Text(
                        if (BluetoothPairingRepairPolicy.requiresAndroidForget(repairCode)) {
                            "Open Bluetooth settings"
                        } else "Repair Bluetooth pairing",
                    )
                }
            }
            repairCode == BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING -> Column(
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                StatusPill("Controller missing", StatusTone.NEGATIVE)
                Text(
                    "The new Android bond works, but Kitsu rejected this phone's saved controller authorization. No controller was added or replaced. Check CONNECT > CONTROLLERS on Kitsu before choosing whether to issue a new authorization.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.testTag("saved-controller-missing"),
                )
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
    pairingFlow: ControllerPairingFlow,
    onPairingFlowChange: (ControllerPairingFlow) -> Unit,
    onPairController: (String) -> Unit,
    onPairCaretakerController: (String) -> Unit,
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
                val pendingRole = ControllerPairingPresentation.savedRoleLabel(
                    owner.pendingPairing.role,
                )
                StatusPill("$pendingRole authorization", StatusTone.ACTIVE)
                Text(
                    "$pendingRole pairing with ${owner.pendingPairing.displayName} may already be committed on the device. Keep it nearby and finish the recovery step.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(
                    onClick = onFinishPairing,
                    enabled = !owner.pairing && !updateBusy,
                    modifier = Modifier.testTag("pairing-finish"),
                ) {
                    Text(if (owner.pairing) "Finishing…" else "Finish ${pendingRole.lowercase()} pairing")
                }
            }
            owner.pairing -> {
                val pairingCopy = ControllerPairingPresentation.copy(pairingFlow)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator()
                    Column {
                        Text(pairingCopy.progressLabel, style = MaterialTheme.typography.titleMedium)
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
                    "Choose the matching option after selecting the pairing role physically on Kitsu.",
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
                val ownerCopy = ControllerPairingPresentation.copy(ControllerPairingFlow.OWNER)
                Text("Owner phone", style = MaterialTheme.typography.titleMedium)
                Text(
                    ownerCopy.deviceInstruction,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    ownerCopy.accessSummary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(
                    onClick = {
                        onPairingFlowChange(ControllerPairingFlow.OWNER)
                        onPairController(pairingLabel)
                    },
                    enabled = !updateBusy && owner.savedKitsu.size < MAX_SAVED_KITSU,
                    modifier = Modifier.fillMaxWidth().testTag("pairing-start"),
                ) { Text(ownerCopy.buttonLabel) }
                HorizontalDivider()
                val caretakerCopy = ControllerPairingPresentation.copy(ControllerPairingFlow.CARETAKER)
                Text("Caretaker phone", style = MaterialTheme.typography.titleMedium)
                Text(
                    caretakerCopy.deviceInstruction,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    caretakerCopy.accessSummary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedButton(
                    onClick = {
                        onPairingFlowChange(ControllerPairingFlow.CARETAKER)
                        onPairCaretakerController(pairingLabel)
                    },
                    enabled = !updateBusy && owner.savedKitsu.size < MAX_SAVED_KITSU,
                    modifier = Modifier.fillMaxWidth().testTag("pairing-start-caretaker"),
                ) { Text(caretakerCopy.buttonLabel) }
                if (owner.savedKitsu.size >= MAX_SAVED_KITSU) {
                    Text(
                        "The saved-device limit is full. Repair Bluetooth pairing for a saved Kitsu, or forget one saved controller before pairing a different Kitsu.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
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
private fun LocalAutomationCard(
    capabilityToken: String?,
    enabled: Boolean,
    onEnable: () -> Unit,
    onDisable: () -> Unit,
    onCopyToken: (String) -> Unit,
) {
    KitsuCard(
        title = "Shortcuts & local automation",
        modifier = Modifier.testTag("settings-local-automation"),
    ) {
        Text(
            "Launcher shortcuts open a visible confirmation. Optional Tasker access can use the app's local companion actions after you enable a private capability on this phone.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Mesh messages, firmware, pairings, unlock codes, and settings are never exposed to automation.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (capabilityToken == null) {
            Button(onClick = onEnable, enabled = enabled) { Text("Enable Tasker access") }
        } else {
            StatusPill("Tasker access enabled", StatusTone.POSITIVE)
            Text(
                "Tasker: launch an Activity with component " +
                    "${BuildConfig.APPLICATION_ID}/ptl.kitsu.app.automation.KitsuAutomationActivity, " +
                    "action ${KitsuAutomationPolicy.ACTION_AUTOMATE}, and the two String extras shown in the copied setup.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "Allowed action values: " +
                    KitsuAutomationAction.entries.joinToString { it.wireName },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = {
                        onCopyToken(
                            KitsuAutomationPolicy.setupRecipe(
                                BuildConfig.APPLICATION_ID,
                                capabilityToken,
                            ),
                        )
                    },
                    enabled = enabled,
                ) { Text("Copy Tasker setup") }
                TextButton(onClick = onDisable, enabled = enabled) { Text("Revoke") }
            }
        }
    }
}

@Composable
private fun NotificationSettingsCard(
    owner: OwnerState,
    settings: KitsuNotificationSettings,
    directMessagesVisible: Boolean,
    enabled: Boolean,
    onEnableAlerts: () -> Unit,
    onDisableAlerts: () -> Unit,
    onDirectMessagesChanged: (Boolean) -> Unit,
    onPetUpdatesChanged: (Boolean) -> Unit,
    onEnableContinuity: () -> Unit,
    onDisableContinuity: () -> Unit,
) {
    KitsuCard(
        title = "Notifications & connection",
        modifier = Modifier.testTag("notification-settings-card"),
    ) {
        Text(
            if (directMessagesVisible) {
                "Everything here is opt-in. Message and pet details stay private on the lock screen."
            } else {
                "Everything here is opt-in. Companion details stay private on the lock screen."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        NotificationToggleRow(
            title = "Kitsu notifications",
            supporting = if (directMessagesVisible) {
                "Allow direct-message and companion updates on this phone"
            } else {
                "Allow companion updates on this caretaker phone"
            },
            checked = settings.alertsEnabled,
            enabled = enabled,
            testTag = "notifications-enabled-switch",
            onCheckedChange = { checked ->
                if (checked) onEnableAlerts() else onDisableAlerts()
            },
        )
        if (directMessagesVisible) {
            NotificationToggleRow(
                title = "Direct messages",
                supporting = "Notify only for new direct journal entries; blocked peers stay hidden",
                checked = settings.directMessagesEnabled,
                enabled = enabled && settings.alertsEnabled,
                testTag = "direct-message-notifications-switch",
                onCheckedChange = onDirectMessagesChanged,
            )
        }
        NotificationToggleRow(
            title = "Companion updates",
            supporting = "Requests, check-ins, focus changes and walk moments",
            checked = settings.petUpdatesEnabled,
            enabled = enabled && settings.alertsEnabled,
            testTag = "pet-update-notifications-switch",
            onCheckedChange = onPetUpdatesChanged,
        )
        HorizontalDivider()
        NotificationToggleRow(
            title = "Keep current connection active",
            supporting = when {
                !settings.alertsEnabled -> "Enable Kitsu notifications first"
                !owner.connection.connected -> "Connect the selected Kitsu first"
                settings.connectionContinuityEnabled -> "Active until you turn it off or Kitsu disconnects"
                else -> "Starts only from this switch; never at boot and never reconnects silently"
            },
            checked = settings.connectionContinuityEnabled,
            enabled = enabled && settings.alertsEnabled && owner.connection.connected,
            testTag = "connection-continuity-switch",
            onCheckedChange = { checked ->
                if (checked) onEnableContinuity() else onDisableContinuity()
            },
        )
    }
}

@Composable
private fun NotificationToggleRow(
    title: String,
    supporting: String,
    checked: Boolean,
    enabled: Boolean,
    testTag: String,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(
                supporting,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            enabled = enabled,
            modifier = Modifier.testTag(testTag),
        )
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
    var confirmReinstall by rememberSaveable { mutableStateOf(false) }
    val installAction = firmwareInstallAction(firmware.progress.stage)
    if (confirmReinstall) {
        AlertDialog(
            onDismissRequest = { confirmReinstall = false },
            title = { Text("Install this firmware again?") },
            text = {
                Text(
                    "This writes the same verified firmware to Kitsu's other OTA slot and reboots it. Continue only when you intend to validate both rollback slots.",
                )
            },
            confirmButton = {
                Button(
                    onClick = {
                        confirmReinstall = false
                        viewModel.installImportedFirmware(reinstallConfirmed = true)
                    },
                    enabled = owner.connection.connected && !updateBusy,
                    modifier = Modifier.testTag("firmware-reinstall-confirm"),
                ) { Text("Install again") }
            },
            dismissButton = {
                TextButton(onClick = { confirmReinstall = false }) { Text("Cancel") }
            },
        )
    }
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
        if (installAction != null) {
            Button(
                onClick = {
                    if (installAction.requiresExplicitConfirmation) {
                        confirmReinstall = true
                    } else {
                        viewModel.installImportedFirmware()
                    }
                },
                enabled = firmware.updateId != null && owner.connection.connected && !updateBusy,
                modifier = Modifier.fillMaxWidth().testTag("firmware-install"),
            ) { Text(installAction.label) }
        }
        if (firmware.progress.stage == FirmwareInstallStage.FAILED && firmware.updateId != null) {
            OutlinedButton(
                onClick = viewModel::resetInterruptedFirmwareUpdate,
                enabled = owner.connection.connected && !updateBusy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Reset interrupted update") }
        }
        if (!owner.connection.connected && installAction != null) {
            Text(
                "Connect the selected Kitsu before installing.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

internal data class FirmwareInstallAction(
    val label: String,
    val requiresExplicitConfirmation: Boolean,
)

internal fun firmwareInstallAction(stage: FirmwareInstallStage): FirmwareInstallAction? = when (stage) {
    FirmwareInstallStage.IMPORTED,
    FirmwareInstallStage.FAILED -> FirmwareInstallAction(
        label = "Install on selected Kitsu",
        requiresExplicitConfirmation = false,
    )
    FirmwareInstallStage.COMPLETE -> FirmwareInstallAction(
        label = "Install again on other slot",
        requiresExplicitConfirmation = true,
    )
    else -> null
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
