package app.kitsu.mobile.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Dns
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import app.kitsu.mobile.GatewayEnrollmentStopDecision
import app.kitsu.mobile.MainViewModel
import app.kitsu.mobile.canStartNewEnrollment
import app.kitsu.mobile.stopDecision
import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiSecurity
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.GatewayCatalogPolicy
import app.kitsu.mobile.transport.ProvisioningPolicy

@Composable
internal fun SecureProvisioningSettings(
    ownerState: OwnerState,
    wifiState: MainViewModel.ProvisioningState,
    wifiRetryState: MainViewModel.WifiRetryState,
    gatewayState: MainViewModel.ProvisioningState,
    enrollmentState: MainViewModel.GatewayEnrollmentState,
    remoteAccountSignedIn: Boolean,
    onSaveWifi: (String, String, WifiSecurity) -> Unit,
    onRetryWifi: () -> Unit,
    onSaveGateway: (GatewayConfiguration) -> Unit,
    onConnectRemote: () -> Unit,
    onRefreshGatewayCatalog: () -> Unit,
    onEnrollGateway: (String) -> Unit,
    onStopEnrollmentBeforeConfirmation: () -> Unit,
) {
    // Provisioning inputs intentionally use remember, never rememberSaveable: they must
    // not enter Android saved state, a cache, logs, or disk-backed application storage.
    var ssid by remember { mutableStateOf("") }
    var passphrase by remember { mutableStateOf("") }
    var security by rememberSaveable { mutableStateOf(WifiSecurity.WPA2_WPA3) }
    var advancedGatewayExpanded by rememberSaveable { mutableStateOf(false) }
    var selectedGatewayId by rememberSaveable { mutableStateOf<String?>(null) }
    var gatewayId by remember { mutableStateOf("") }
    var host by remember { mutableStateOf("") }
    var bootstrapPort by remember { mutableStateOf("7442") }
    var port by remember { mutableStateOf("7443") }
    var serverName by remember { mutableStateOf("") }
    var caDerB64 by remember { mutableStateOf("") }
    var spkiB64 by remember { mutableStateOf("") }
    var companionDisplayName by rememberSaveable { mutableStateOf("") }
    val direct = ownerState.connection.connected && ownerState.connection.mode == ConnectionMode.DIRECT_BLE
    val remoteReady = wifiRemoteHandoffReady(ownerState)
    val uriHandler = LocalUriHandler.current
    val selectedGateway = ownerState.gatewayProvisioningRecords.firstOrNull {
        it.gatewayId == selectedGatewayId
    }

    LaunchedEffect(direct) {
        if (direct) {
            onRefreshGatewayCatalog()
        } else {
            ssid = ""
            passphrase = ""
            gatewayId = ""
            host = ""
            bootstrapPort = "7442"
            port = "7443"
            serverName = ""
            caDerB64 = ""
            spkiB64 = ""
        }
    }
    LaunchedEffect(ownerState.gatewayProvisioningRecords) {
        if (ownerState.gatewayProvisioningRecords.none { it.gatewayId == selectedGatewayId }) {
            selectedGatewayId = ownerState.gatewayProvisioningRecords.firstOrNull()?.gatewayId
        }
    }
    LaunchedEffect(wifiState) {
        if (wifiState == MainViewModel.ProvisioningState.Stored ||
            wifiState is MainViewModel.ProvisioningState.AcceptedUnverified
        ) {
            // A signed accepted receipt means the secret has already left this
            // form even when the follow-up state read cannot be verified.
            ssid = ""
            passphrase = ""
        }
    }
    LaunchedEffect(gatewayState) {
        if (gatewayState == MainViewModel.ProvisioningState.Stored) {
            gatewayId = ""
            host = ""
            bootstrapPort = "7442"
            port = "7443"
            serverName = ""
            caDerB64 = ""
            spkiB64 = ""
        }
    }

    val wifiInput = WifiProvisioning(ssid, passphrase, security)
    val wifiError = remember(ssid, passphrase, security) { ProvisioningPolicy.wifiError(wifiInput) }
    val gatewayInput = GatewayConfiguration(
        gatewayId = gatewayId,
        host = host,
        bootstrapPort = bootstrapPort.toIntOrNull() ?: 0,
        port = port.toIntOrNull() ?: 0,
        serverName = serverName,
        caCertificateDerB64 = caDerB64,
        spkiSha256B64 = spkiB64,
    )
    val gatewayError = remember(gatewayId, host, bootstrapPort, port, serverName, caDerB64, spkiB64) {
        ProvisioningPolicy.gatewayError(gatewayInput)
    }

    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        ProvisioningHeading(
            "Wi-Fi and home gateway",
            "Configuration is sent only through the authenticated encrypted Bluetooth session. " +
                "Kitsu stores it in its protected device store; this app does not save these inputs.",
        )
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Text("Device status", fontWeight = FontWeight.SemiBold)
                val wifiPresentation = wifiStatusPresentation(ownerState)
                Text("Wi-Fi credentials: ${wifiPresentation.credentials}")
                Text("Wi-Fi link: ${wifiPresentation.link}")
                Text("LAN gateway: ${gatewayLanStatusLabel(ownerState.status?.lan?.lanState)}")
                Text(
                    "Enrollment: " + gatewayEnrollmentStatusLabel(
                        ownerState.status?.lan?.gatewayEnrollmentState,
                    ),
                )
                Text(
                    "Remote security: " + when (ownerState.status?.lan?.remoteConnectivityAllowed) {
                        true -> "ready"
                        false -> "not ready"
                        null -> "unknown"
                    },
                )
                Text(
                    if (ownerState.status?.lan?.gatewayEnrolled == true) {
                        "Mutual TLS identity enrolled"
                    } else {
                        "Mutual TLS identity not enrolled yet"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        OutlinedButton(
            onClick = onRetryWifi,
            enabled = direct && ownerState.status?.lan?.wifiConfigured == true &&
                wifiRetryState != MainViewModel.WifiRetryState.Retrying,
            modifier = Modifier.fillMaxWidth().testTag("wifi-retry"),
        ) {
            if (wifiRetryState == MainViewModel.WifiRetryState.Retrying) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
            } else {
                Icon(Icons.Filled.Wifi, contentDescription = null)
            }
            Spacer(Modifier.size(8.dp))
            Text(
                if (wifiRetryState == MainViewModel.WifiRetryState.Retrying) {
                    "Restarting Kitsu Wi-Fi…"
                } else {
                    "Retry Kitsu Wi-Fi now"
                },
            )
        }
        when (wifiRetryState) {
            MainViewModel.WifiRetryState.Idle -> Unit
            MainViewModel.WifiRetryState.Retrying -> Text(
                "Kitsu is reloading its stored Wi-Fi profile.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            MainViewModel.WifiRetryState.Accepted -> Text(
                "Retry accepted. Wi-Fi link: ${wifiStatusPresentation(ownerState).link}.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.tertiary,
                modifier = Modifier.testTag("wifi-retry-result"),
            )
            is MainViewModel.WifiRetryState.Failed -> Text(
                provisioningLabel(wifiRetryState.code),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.testTag("wifi-retry-result"),
            )
        }
        ProvisioningHeading(
            "Home Wi-Fi",
            "SSID is 1–32 UTF-8 bytes. Passphrase is 8–63 printable ASCII characters.",
        )
        OutlinedTextField(
            value = ssid,
            onValueChange = { candidate ->
                if (candidate.toByteArray(Charsets.UTF_8).size <= 32 && '\u0000' !in candidate) ssid = candidate
            },
            label = { Text("Wi-Fi network name (SSID)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth().testTag("wifi-ssid"),
        )
        LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            item {
                FilterChip(
                    selected = security == WifiSecurity.WPA2,
                    onClick = { security = WifiSecurity.WPA2 },
                    label = { Text("WPA2") },
                )
            }
            item {
                FilterChip(
                    selected = security == WifiSecurity.WPA2_WPA3,
                    onClick = { security = WifiSecurity.WPA2_WPA3 },
                    label = { Text("WPA2/WPA3") },
                )
            }
            item {
                FilterChip(
                    selected = security == WifiSecurity.WPA3,
                    onClick = { security = WifiSecurity.WPA3 },
                    label = { Text("WPA3") },
                )
            }
        }
        OutlinedTextField(
            value = passphrase,
            onValueChange = { candidate ->
                if (candidate.length <= 63 && candidate.all { it.code in 0x20..0x7e }) passphrase = candidate
            },
            label = { Text("Wi-Fi passphrase") },
            visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Password,
                imeAction = ImeAction.Done,
            ),
            singleLine = true,
            supportingText = { Text("${passphrase.length} / 63 characters") },
            modifier = Modifier.fillMaxWidth().testTag("wifi-passphrase"),
        )
        Button(
            onClick = { onSaveWifi(ssid, passphrase, security) },
            enabled = direct && wifiError == null && wifiState != MainViewModel.ProvisioningState.Saving,
            modifier = Modifier.fillMaxWidth().testTag("wifi-save"),
        ) {
            if (wifiState == MainViewModel.ProvisioningState.Saving) {
                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
            } else {
                Icon(Icons.Filled.Wifi, contentDescription = null)
            }
            Spacer(Modifier.size(8.dp))
            Text("Store Wi-Fi on Kitsu")
        }
        ProvisioningFeedback(wifiState, wifiError, ssid.isNotEmpty() || passphrase.isNotEmpty())
        wifiStatusPresentation(ownerState).nextStep?.let { nextStep ->
            Text(
                nextStep,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("wifi-next-step"),
            )
        }
        if (direct && remoteReady && remoteAccountSignedIn) {
            OutlinedButton(
                onClick = onConnectRemote,
                modifier = Modifier.fillMaxWidth().testTag("wifi-use-remote"),
            ) {
                Icon(Icons.Filled.Wifi, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text("End Bluetooth and use Wi-Fi remote")
            }
        } else if (direct && remoteReady) {
            Text(
                "Sign in to the owner account before ending Bluetooth for remote access.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        ProvisioningHeading(
            "Home gateway",
            "Choose a gateway from your authenticated Kitsu account. Its verified address and public " +
                "trust anchors are transferred directly to Kitsu over encrypted Bluetooth.",
        )
        when {
            ownerState.gatewayCatalogLoading -> {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text("Loading your gateways...")
                }
            }
            ownerState.gatewayProvisioningRecords.isNotEmpty() -> {
                ownerState.gatewayProvisioningRecords.forEach { record ->
                    FilterChip(
                        selected = record.gatewayId == selectedGatewayId,
                        onClick = { selectedGatewayId = record.gatewayId },
                        label = {
                            Column {
                                Text(record.displayName, fontWeight = FontWeight.SemiBold)
                                Text(
                                    "${record.host}:${record.bootstrapPort} bootstrap / " +
                                        "${record.port} mTLS | ${record.state.replace('_', ' ')}",
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                        },
                        modifier = Modifier.fillMaxWidth().testTag("gateway-record-${record.gatewayId}"),
                    )
                }
            }
            ownerState.gatewayCatalogError == "sign_in_required" -> {
                Text(
                    "Sign in to your Kitsu owner account to load trusted gateways.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            else -> {
                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(
                        Modifier.padding(14.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        Text("No gateway is available", fontWeight = FontWeight.SemiBold)
                        Text(
                            "Complete Wi-Fi and gateway setup, then refresh this list. " +
                                "Advanced setup is only for a server you operate.",
                            style = MaterialTheme.typography.bodySmall,
                        )
                        TextButton(
                            onClick = { uriHandler.openUri(GatewayCatalogPolicy.GATEWAY_SETUP_URL) },
                            modifier = Modifier.testTag("gateway-setup-guide"),
                        ) {
                            Text("Open gateway setup guide")
                        }
                    }
                }
            }
        }
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(
                onClick = onRefreshGatewayCatalog,
                enabled = !ownerState.gatewayCatalogLoading,
                modifier = Modifier.weight(1f).testTag("gateway-refresh"),
            ) {
                Text("Refresh")
            }
            Button(
                onClick = {
                    selectedGateway?.let { onSaveGateway(it.toGatewayConfiguration()) }
                },
                enabled = direct && selectedGateway != null &&
                    gatewayState != MainViewModel.ProvisioningState.Saving,
                modifier = Modifier.weight(1f).testTag("gateway-catalog-save"),
            ) {
                if (gatewayState == MainViewModel.ProvisioningState.Saving) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                } else {
                    Icon(Icons.Filled.Save, contentDescription = null)
                }
                Spacer(Modifier.size(8.dp))
                Text("Store on Kitsu")
            }
        }
        if (selectedGateway != null) {
            ProvisioningFeedback(gatewayState, null, touched = true)
        }

        OutlinedButton(
            onClick = { advancedGatewayExpanded = !advancedGatewayExpanded },
            modifier = Modifier.fillMaxWidth().testTag("gateway-advanced-toggle"),
        ) {
            Icon(Icons.Filled.Dns, contentDescription = null)
            Spacer(Modifier.size(8.dp))
            Text(if (advancedGatewayExpanded) "Hide Advanced setup" else "Advanced: self-hosted gateway")
        }
        if (advancedGatewayExpanded) {
            ProvisioningHeading(
                "Advanced gateway trust",
                "The device creates and retains its own mTLS key, certificate, and backend secret. " +
                    "Only use these manual fields for a server you operate and whose CA and SPKI pin you verified.",
            )
            OutlinedTextField(
                value = gatewayId,
                onValueChange = { gatewayId = it.trim() },
                label = { Text("Gateway ID (lowercase UUID)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = host,
                onValueChange = { host = it.trim() },
                label = { Text("LAN host, .local name, or IP") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = bootstrapPort,
                onValueChange = { if (it.length <= 5 && it.all(Char::isDigit)) bootstrapPort = it },
                label = { Text("Bootstrap port (no client certificate)") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = port,
                onValueChange = { if (it.length <= 5 && it.all(Char::isDigit)) port = it },
                label = { Text("Steady mTLS port") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = serverName,
                onValueChange = { serverName = it.trim() },
                label = { Text("TLS server name") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = caDerB64,
                onValueChange = { caDerB64 = it.filterNot(Char::isWhitespace) },
                label = { Text("X.509 CA DER (unpadded base64url)") },
                minLines = 2,
                maxLines = 4,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = spkiB64,
                onValueChange = { spkiB64 = it.filterNot(Char::isWhitespace) },
                label = { Text("SPKI SHA-256 (unpadded base64url)") },
                minLines = 1,
                maxLines = 2,
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = { onSaveGateway(gatewayInput) },
                enabled = direct && gatewayError == null &&
                    gatewayState != MainViewModel.ProvisioningState.Saving,
                modifier = Modifier.fillMaxWidth().testTag("gateway-save"),
            ) {
                if (gatewayState == MainViewModel.ProvisioningState.Saving) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                } else {
                    Icon(Icons.Filled.Save, contentDescription = null)
                }
                Spacer(Modifier.size(8.dp))
                Text("Store manual trust on Kitsu")
            }
            ProvisioningFeedback(
                gatewayState,
                gatewayError,
                listOf(gatewayId, host, serverName, caDerB64, spkiB64).any(String::isNotEmpty),
            )
        }
        HorizontalDivider(Modifier.padding(vertical = 4.dp))
        ProvisioningHeading(
            "Owner enrollment",
            "Sign in, keep the authenticated Bluetooth session connected, and confirm this one-time " +
                "claim physically on Kitsu. The claim token is never displayed, cached, or logged.",
        )
        OutlinedTextField(
            value = companionDisplayName,
            onValueChange = { candidate ->
                if (candidate.toByteArray(Charsets.UTF_8).size <= 80 &&
                    candidate.none(Char::isISOControl)
                ) companionDisplayName = candidate
            },
            enabled = enrollmentState.canStartNewEnrollment(),
            label = { Text("Companion name in your account") },
            supportingText = {
                Text("${companionDisplayName.toByteArray(Charsets.UTF_8).size} / 80 UTF-8 bytes")
            },
            singleLine = true,
            modifier = Modifier.fillMaxWidth().testTag("enrollment-display-name"),
        )
        GatewayEnrollmentFeedback(enrollmentState)
        val canStopBeforeConfirmation = enrollmentState.stopDecision() ==
            GatewayEnrollmentStopDecision.STOP_AND_DISCONNECT
        if (canStopBeforeConfirmation) {
            OutlinedButton(
                onClick = onStopEnrollmentBeforeConfirmation,
                modifier = Modifier.fillMaxWidth().testTag("enrollment-stop-before-confirmation"),
            ) {
                Text("Stop and disconnect")
            }
        } else if (enrollmentState.canStartNewEnrollment() &&
            ownerState.status?.lan?.gatewayEnrolled != true
        ) {
            Button(
                onClick = { onEnrollGateway(companionDisplayName) },
                enabled = direct && ownerState.status?.lan?.gatewayConfigured == true &&
                    ownerState.status?.lan?.wifiConfigured == true &&
                    ownerState.status?.lan?.remoteConnectivityAllowed == true &&
                    ownerState.provisionedGatewayId != null &&
                    companionDisplayName.trim().toByteArray(Charsets.UTF_8).size in 1..80,
                modifier = Modifier.fillMaxWidth().testTag("enrollment-start"),
            ) {
                Icon(Icons.Filled.Lock, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text("Enroll this Kitsu")
            }
        }
        Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Row(Modifier.padding(14.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Icon(Icons.Filled.Lock, contentDescription = null)
                Text(
                    "Before the app reports physical acceptance, Stop disconnects immediately and an " +
                        "unaccepted claim expires. If PRG acceptance races that disconnect, Kitsu may " +
                        "already be committed; reconnect to verify. Once acceptance is reported, the " +
                        "bootstrap cannot be cancelled from this app.",
                    Modifier.weight(1f),
                )
            }
        }
    }
}

internal data class GatewayEnrollmentPresentation(
    val title: String,
    val detail: String,
    val error: Boolean,
)

internal data class WifiStatusPresentation(
    val credentials: String,
    val link: String,
    val nextStep: String?,
)

internal fun wifiRemoteHandoffReady(ownerState: OwnerState): Boolean {
    val lan = ownerState.status?.lan ?: return false
    return lan.wifiState == "connected" && lan.wifiConfigured == true && lan.gatewayConfigured == true &&
        lan.gatewayEnrolled == true && lan.remoteConnectivityAllowed == true &&
        lan.lanState in setOf("ble_priority", "connected")
}

private fun gatewayLanStatusLabel(state: String?): String = when (state) {
    null -> "unknown"
    "config_unavailable" -> "configuration storage unavailable"
    "connectivity_unavailable" -> "remote connectivity unavailable"
    "unconfigured" -> "not configured"
    "ble_priority" -> "waiting for Bluetooth handoff"
    "wifi_pending" -> "waiting for Wi-Fi"
    "enrollment_pending" -> "enrollment required"
    "time_pending" -> "waiting for trusted time"
    "replay_unavailable" -> "message replay unavailable"
    "reconnecting" -> "connecting"
    "connected" -> "connected"
    else -> state.replace('_', ' ')
}

private fun gatewayEnrollmentStatusLabel(state: String?): String = when (state) {
    null -> "unknown"
    "idle" -> "not started"
    "physical_confirmation_required" -> "waiting for confirmation on Kitsu"
    "physical_confirmed" -> "confirmed on Kitsu"
    "ready_for_wifi" -> "ready for Wi-Fi handoff"
    "bootstrapping" -> "registering through the gateway"
    "enrolled" -> "enrolled"
    "failed" -> "failed"
    "expired" -> "confirmation expired"
    else -> state.replace('_', ' ')
}

/** Keeps stored configuration separate from the live radio state. Current
 * firmware can associate Wi-Fi before gateway setup and keeps STA warm during
 * BLE; older firmware states are still explained without misreporting storage. */
internal fun wifiStatusPresentation(ownerState: OwnerState): WifiStatusPresentation {
    val lan = ownerState.status?.lan ?: return WifiStatusPresentation(
        credentials = "unknown",
        link = "unknown",
        nextStep = "Connect directly over Bluetooth and refresh before entering Wi-Fi again.",
    )
    val credentials = when (lan.wifiConfigured) {
        true -> "stored and verified"
        false -> "not stored"
        null -> "unknown"
    }
    val link = when (lan.wifiState) {
        "unknown" -> "unknown"
        "unconfigured" -> when {
            lan.wifiConfigured != true -> "not configured"
            lan.remoteConnectivityAllowed != true -> "blocked by device security state"
            else -> "credentials stored; association has not started yet"
        }
        "storage_unavailable" -> "protected storage unavailable"
        "connectivity_unavailable" -> "remote connectivity unavailable"
        "ble_active" -> "paused by the installed firmware while Direct Bluetooth is active"
        "grace" -> "older firmware is waiting after Bluetooth disconnect"
        "connecting" -> "joining the configured network"
        "connected" -> "connected"
        "backoff" -> "association failed; retry scheduled"
        else -> lan.wifiState.replace('_', ' ')
    }
    val nextStep = when {
        lan.wifiConfigured != true ->
            "Store Wi-Fi once. Android will confirm the device-reported stored flag before calling it verified."
        lan.remoteConnectivityAllowed != true ->
            "Wi-Fi remote access is blocked until Kitsu reports its protected connectivity store is ready."
        lan.gatewayConfigured != true ->
            "Kitsu can join Wi-Fi now. Next: choose and store gateway trust to enable remote access."
        lan.gatewayEnrolled != true ->
            "Next: enroll this Kitsu. After physical confirmation Android ends BLE, unlocks the gateway bootstrap, and checks the owner service."
        ownerState.connection.mode == ConnectionMode.REMOTE_BACKEND ->
            "Android is using the authenticated owner service; Kitsu reaches it through its Wi-Fi gateway."
        lan.wifiState == "ble_active" ->
            "This installed firmware pauses Wi-Fi for Direct Bluetooth. Update Kitsu firmware for reliable in-app remote handoff."
        lan.wifiState != "connected" ->
            "Wi-Fi is not connected yet. Keep Direct Bluetooth active while Kitsu retries and tap Refresh to update this status; if it stays in backoff, verify the network details before storing them again."
        ownerState.connection.mode == ConnectionMode.DIRECT_BLE ->
            "Remote setup is complete. Use the button below to end BLE and connect through the owner service."
        else -> null
    }
    return WifiStatusPresentation(credentials, link, nextStep)
}

internal fun gatewayEnrollmentPresentation(
    state: MainViewModel.GatewayEnrollmentState,
): GatewayEnrollmentPresentation = when (state) {
        MainViewModel.GatewayEnrollmentState.Idle -> GatewayEnrollmentPresentation(
            "Ready when gateway trust is stored",
            "Enrollment creates a short-lived owner claim. Starting it will require a PRG hold on Kitsu.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.CreatingClaim -> GatewayEnrollmentPresentation(
            "Creating protected owner claim…",
            "Keep Kitsu nearby. You can still stop safely before physical confirmation.",
            false,
        )
        is MainViewModel.GatewayEnrollmentState.WaitingForPhysicalConfirmation ->
            GatewayEnrollmentPresentation(
            "Hold PRG on Kitsu now",
            "Kitsu must show the enrollment confirmation screen. About " +
                "${(state.remainingMilliseconds + 999) / 1_000} seconds remain. Stop is available only " +
                "until Kitsu accepts the physical confirmation.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.SwitchingToWifi -> GatewayEnrollmentPresentation(
            "Physical confirmation accepted",
            "Kitsu committed the device bootstrap. It cannot be cancelled from this app; BLE is ending " +
                "and the already-associated Wi-Fi link is being handed to the configured gateway.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.PollingBackend -> GatewayEnrollmentPresentation(
            "Waiting for Kitsu online…",
            "The committed bootstrap is continuing. The app is checking the authenticated owner service.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.Complete -> GatewayEnrollmentPresentation(
            "Enrollment complete",
            "Kitsu is enrolled with its device-generated mTLS identity.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.CancelledBeforePhysicalConfirmation ->
            GatewayEnrollmentPresentation(
            "Stopped before acceptance was reported",
            "The app disconnected and will not retry. If Kitsu accepted PRG at the same moment, its " +
                "Wi-Fi bootstrap may still continue; reconnect explicitly to verify.",
            false,
        )
        MainViewModel.GatewayEnrollmentState.MonitoringStoppedAfterPhysicalCommit ->
            GatewayEnrollmentPresentation(
            "Monitoring stopped after commit",
            "The app disconnected, but Kitsu already accepted enrollment and may still finish its " +
                "Wi-Fi bootstrap. Reconnect explicitly to verify its state.",
            false,
        )
        is MainViewModel.GatewayEnrollmentState.Failed -> GatewayEnrollmentPresentation(
            if (state.physicalCommitAccepted) {
                "Kitsu accepted enrollment; verification incomplete"
            } else {
                "Enrollment did not reach physical confirmation"
            },
            if (state.physicalCommitAccepted) {
                "The device bootstrap may still complete. ${provisioningLabel(state.code)}"
            } else {
                provisioningLabel(state.code)
            },
            true,
        )
    }

@Composable
private fun GatewayEnrollmentFeedback(state: MainViewModel.GatewayEnrollmentState) {
    val (title, detail, error) = gatewayEnrollmentPresentation(state)
    Card(
        colors = CardDefaults.cardColors(
            containerColor = if (error) {
                MaterialTheme.colorScheme.errorContainer
            } else {
                MaterialTheme.colorScheme.surfaceVariant
            },
        ),
        modifier = Modifier.fillMaxWidth().testTag("enrollment-state"),
    ) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, fontWeight = FontWeight.SemiBold)
            Text(detail, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun ProvisioningHeading(title: String, detail: String) {
    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        Text(
            detail,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun ProvisioningFeedback(
    state: MainViewModel.ProvisioningState,
    validationError: String?,
    touched: Boolean,
) {
    val (text, error) = when (state) {
        MainViewModel.ProvisioningState.Idle -> if (touched && validationError != null) {
            provisioningLabel(validationError) to false
        } else {
            null to false
        }
        MainViewModel.ProvisioningState.Saving -> "Writing and read-back verifying on Kitsu…" to false
        MainViewModel.ProvisioningState.Stored -> "Stored and read-back verified on Kitsu." to false
        is MainViewModel.ProvisioningState.AcceptedUnverified ->
            provisioningLabel(state.code) to true
        is MainViewModel.ProvisioningState.Failed -> provisioningLabel(state.code) to true
    }
    text?.let {
        Text(
            it,
            style = MaterialTheme.typography.bodySmall,
            color = if (error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun provisioningLabel(code: String): String = when (code) {
    "invalid_ssid" -> "SSID must be 1–32 valid UTF-8 bytes and contain no NUL."
    "invalid_passphrase" -> "Passphrase must be 8–63 printable ASCII characters."
    "invalid_gateway_id" -> "Gateway ID must be a canonical lowercase UUID."
    "invalid_host" -> "Host must be a valid hostname, .local name, IPv4, or IPv6 literal without a URL scheme/path."
    "invalid_server_name" -> "TLS server name must be a valid DNS hostname."
    "invalid_bootstrap_port" -> "Bootstrap port must be between 1 and 65535."
    "invalid_port" -> "Port must be between 1 and 65535."
    "gateway_ports_must_differ" -> "Bootstrap and steady mTLS ports must be different."
    "invalid_ca_certificate" -> "CA must be canonical unpadded base64url containing a valid X.509 CA DER certificate (8192 bytes max)."
    "invalid_spki" -> "SPKI pin must be canonical unpadded base64url of exactly 32 bytes."
    "security_unavailable" -> "Kitsu’s protected device store is unavailable in this build."
    "storage_unavailable" -> "Kitsu’s protected device store is unavailable."
    "storage_failed" -> "Kitsu could not securely store and verify this configuration."
    "wifi_storage_not_confirmed" ->
        "Kitsu accepted the write, but its authenticated status did not confirm stored Wi-Fi. Reconnect and check before retrying."
    "wifi_storage_verification_unavailable" ->
        "Kitsu accepted the write, but the authenticated read-back was interrupted. Reconnect and check status before retrying."
    "wifi_configuration_rejected" -> "Kitsu rejected the Wi-Fi configuration."
    "wifi_unconfigured" -> "Store Wi-Fi on Kitsu before retrying the connection."
    "connectivity_unavailable" -> "Wi-Fi is unavailable in the installed firmware build."
    "wifi_retry_rejected" -> "Kitsu rejected the Wi-Fi retry."
    "request_rejected", "unsupported_operation", "unsupported_protocol" ->
        "Install the latest Kitsu firmware to use immediate Wi-Fi retry."
    "direct_ble_required" -> "Connect directly over Bluetooth to provision this Kitsu."
    "sign_in_required" -> "Sign in to create an owner enrollment claim."
    "not_configured" -> "Store gateway trust on Kitsu before enrollment."
    "wifi_not_configured" -> "Store Wi-Fi on Kitsu before enrollment."
    "remote_connectivity_not_allowed" -> "Kitsu's application security store is not ready for remote connectivity."
    "gateway_identity_unknown" -> "Select and store the exact gateway again before enrollment."
    "already_enrolled" -> "This Kitsu already has an enrolled device identity."
    "time_unset" -> "Kitsu could not establish trusted time. Reconnect and try again."
    "busy" -> "Kitsu is already handling another protected operation."
    "expired" -> "The physical-confirmation window expired. Start a new enrollment."
    "physical_confirmation_required" -> "Hold PRG on Kitsu while the confirmation window is open."
    "bootstrap_failed" -> "The app could not verify bootstrap through the configured gateway."
    "backend_poll_not_authorized" -> "App-side authenticated backend polling is no longer authorized."
    "remote_companion_offline" -> "Kitsu has not established its authenticated remote path yet."
    "remote_provenance_unverified", "remote_gateway_unverified", "remote_last_seen_invalid" ->
        "The backend could not prove a current device-authenticated gateway path."
    "remote_companion_binding_failed", "remote_gateway_binding_failed" ->
        "The remote device or gateway did not match the enrollment just confirmed on Kitsu."
    else -> code.replace('_', ' ').replaceFirstChar(Char::uppercase)
}
