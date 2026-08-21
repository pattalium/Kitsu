package app.kitsu.mobile.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BluetoothConnected
import androidx.compose.material.icons.filled.Campaign
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.CloudOff
import androidx.compose.material.icons.filled.DoneAll
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Hub
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.LinkOff
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.MarkEmailUnread
import androidx.compose.material.icons.filled.Pets
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Restaurant
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.AssistChip
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.kitsu.mobile.MainViewModel
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.MessageRoute
import app.kitsu.mobile.relay.MobileRelayUiState
import app.kitsu.mobile.repository.OwnerState
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.RemoteSnapshotPolicy
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private enum class OwnerTab(
    val title: String,
    val navigationLabel: String,
    val icon: ImageVector,
) {
    HOME("Companion", "Home", Icons.Filled.Home),
    MESSAGES("Messages", "Chat", Icons.AutoMirrored.Filled.Chat),
    NETWORK("Mesh network", "Mesh", Icons.Filled.Hub),
    CARE("Care and actions", "Care", Icons.Filled.Favorite),
    SETTINGS("Settings", "More", Icons.Filled.Settings),
}

private val KitsuColors = darkColorScheme(
    primary = Color(0xFFFFA26B),
    onPrimary = Color(0xFF301208),
    secondary = Color(0xFFFFD0A9),
    tertiary = Color(0xFFB9DCCB),
    surface = Color(0xFF211A17),
    surfaceVariant = Color(0xFF332824),
    background = Color(0xFF120F0D),
    error = Color(0xFFFFB4AB),
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun KitsuOwnerApp(
    viewModel: MainViewModel,
    onRequestBlePermissions: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onPairController: (String) -> Unit,
    onOpenAppSettings: () -> Unit,
    onSignIn: () -> Unit,
    onSignOut: () -> Unit,
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val notice by viewModel.notice.collectAsStateWithLifecycle()
    val messageSendState by viewModel.messageSendState.collectAsStateWithLifecycle()
    val wifiProvisioningState by viewModel.wifiProvisioningState.collectAsStateWithLifecycle()
    val wifiRetryState by viewModel.wifiRetryState.collectAsStateWithLifecycle()
    val gatewayProvisioningState by viewModel.gatewayProvisioningState.collectAsStateWithLifecycle()
    val gatewayEnrollmentState by viewModel.gatewayEnrollmentState.collectAsStateWithLifecycle()
    val ownerAccountStatus by viewModel.ownerAccountStatus.collectAsStateWithLifecycle()
    val mobileRelayState by viewModel.mobileRelayState.collectAsStateWithLifecycle()
    var tab by rememberSaveable { mutableStateOf(OwnerTab.HOME) }
    val snackbar = remember { SnackbarHostState() }
    val unreadMessages = MessageComposerPolicy.unreadCount(state.messages)

    LaunchedEffect(notice) {
        notice?.let {
            snackbar.showSnackbar(it)
            viewModel.clearNotice()
        }
    }

    MaterialTheme(colorScheme = KitsuColors) {
        Scaffold(
            topBar = {
                CenterAlignedTopAppBar(
                    title = {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Kitsu", fontWeight = FontWeight.Bold)
                            Text(
                                tab.title,
                                style = MaterialTheme.typography.labelMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    },
                    actions = {
                        IconButton(
                            onClick = viewModel::refresh,
                            enabled = state.connection.connected && !state.loading,
                        ) {
                            Icon(Icons.Filled.Refresh, contentDescription = "Refresh companion data")
                        }
                    },
                )
            },
            snackbarHost = { SnackbarHost(snackbar) },
            bottomBar = {
                NavigationBar {
                    OwnerTab.entries.forEach { item ->
                        NavigationBarItem(
                            selected = tab == item,
                            onClick = { tab = item },
                            icon = {
                                if (item == OwnerTab.MESSAGES && unreadMessages > 0) {
                                    BadgedBox(
                                        badge = {
                                            Badge { Text(if (unreadMessages > 99) "99+" else "$unreadMessages") }
                                        },
                                    ) {
                                        Icon(item.icon, contentDescription = null)
                                    }
                                } else {
                                    Icon(item.icon, contentDescription = null)
                                }
                            },
                            label = {
                                Text(
                                    item.navigationLabel,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            },
                            modifier = Modifier.testTag("nav-${item.name.lowercase()}"),
                        )
                    }
                }
            },
        ) { innerPadding ->
            Column(
                Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
                    .consumeWindowInsets(innerPadding),
            ) {
                ConnectionCard(
                    state = state,
                    mobileRelayState = mobileRelayState,
                    ownerAccountStatus = ownerAccountStatus,
                    gatewayEnrollmentState = gatewayEnrollmentState,
                    onConnectNearby = viewModel::reconnectBluetooth,
                    onConnectRemote = viewModel::reconnectRemote,
                    onRequestBlePermissions = onRequestBlePermissions,
                    onOpenAppSettings = onOpenAppSettings,
                    onEnableBluetooth = onEnableBluetooth,
                    onDisconnect = viewModel::disconnect,
                    onCancelPairing = viewModel::cancelPairing,
                )
                when (tab) {
                    OwnerTab.HOME -> HomeScreen(state, viewModel::refresh)
                    OwnerTab.MESSAGES -> MessagesScreen(state, messageSendState, viewModel)
                    OwnerTab.NETWORK -> NetworkScreen(state, viewModel)
                    OwnerTab.CARE -> CareScreen(state, viewModel)
                    OwnerTab.SETTINGS -> SettingsScreen(
                        state = state,
                        viewModel = viewModel,
                        requestPermissions = onRequestBlePermissions,
                        openAppSettings = onOpenAppSettings,
                        enableBluetooth = onEnableBluetooth,
                        pairController = onPairController,
                        signIn = onSignIn,
                        signOut = onSignOut,
                        ownerAccountStatus = ownerAccountStatus,
                        wifiProvisioningState = wifiProvisioningState,
                        wifiRetryState = wifiRetryState,
                        gatewayProvisioningState = gatewayProvisioningState,
                        gatewayEnrollmentState = gatewayEnrollmentState,
                        mobileRelayState = mobileRelayState,
                    )
                }
            }
        }
    }
}

internal data class ConnectionPresentation(
    val title: String,
    val detail: String,
    val color: Color,
    val icon: ImageVector,
)

@Composable
private fun ConnectionCard(
    state: OwnerState,
    mobileRelayState: MobileRelayUiState,
    ownerAccountStatus: OwnerAccountStatus,
    gatewayEnrollmentState: MainViewModel.GatewayEnrollmentState,
    onConnectNearby: () -> Unit,
    onConnectRemote: () -> Unit,
    onRequestBlePermissions: () -> Unit,
    onOpenAppSettings: () -> Unit,
    onEnableBluetooth: () -> Unit,
    onDisconnect: () -> Unit,
    onCancelPairing: () -> Unit,
) {
    val enrollmentInFlight = gatewayEnrollmentInFlight(gatewayEnrollmentState)
    val enrollmentMonitoring = gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.SwitchingToWifi ||
        gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.PollingBackend
    val presentation = when {
        mobileRelayState.enabled -> ConnectionPresentation(
            if (mobileRelayState.detail == "connected_public_gateway") {
                "Public gateway connected"
            } else {
                "Connecting public gateway"
            },
            when (mobileRelayState.detail) {
                "hold_prg_to_connect" -> "Hold PRG on Kitsu to finish connecting."
                "connected_public_gateway" -> "The app is relaying Kitsu over Bluetooth."
                "rate_limited" -> "Paused after too many automatic retries."
                else -> friendlyCode(mobileRelayState.detail)
            },
            Color(0xFF5B477A),
            Icons.Filled.Cloud,
        )
        gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.SwitchingToWifi -> ConnectionPresentation(
            "Switching to Wi-Fi",
            "Kitsu accepted enrollment. Bluetooth is ending so the gateway can take over.",
            Color(0xFF42536F),
            Icons.Filled.Wifi,
        )
        gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.PollingBackend -> ConnectionPresentation(
            "Waiting for the gateway",
            "Checking the authenticated owner service for this Kitsu.",
            Color(0xFF42536F),
            Icons.Filled.Cloud,
        )
        else -> connectionPresentation(state)
    }
    val remoteSignedIn = ownerAccountStatus == OwnerAccountStatus.SIGNED_IN
    val remoteSwitchReady = remoteSignedIn &&
        (state.connection.mode != ConnectionMode.DIRECT_BLE || wifiRemoteHandoffReady(state))
    Card(
        colors = CardDefaults.cardColors(containerColor = presentation.color),
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 8.dp),
    ) {
        Column(
            Modifier.padding(horizontal = 14.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Icon(presentation.icon, contentDescription = null, modifier = Modifier.size(24.dp))
                Column(Modifier.weight(1f)) {
                    Text(presentation.title, fontWeight = FontWeight.SemiBold)
                    Text(
                        presentation.detail,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.82f),
                    )
                }
                if (state.loading || state.connection.mode == ConnectionMode.CONNECTING || enrollmentMonitoring) {
                    CircularProgressIndicator(modifier = Modifier.size(22.dp), strokeWidth = 2.dp)
                }
            }
            when {
                mobileRelayState.enabled -> Unit
                state.pairing -> OutlinedButton(
                    onClick = onCancelPairing,
                    modifier = Modifier.fillMaxWidth().testTag("connection-cancel-pairing"),
                ) {
                    Icon(Icons.Filled.LinkOff, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Cancel pairing")
                }
                enrollmentInFlight -> OutlinedButton(
                    onClick = onDisconnect,
                    modifier = Modifier.fillMaxWidth().testTag("connection-stop-setup"),
                ) {
                    Icon(Icons.Filled.LinkOff, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text(if (enrollmentMonitoring) "Stop monitoring" else "Stop setup")
                }
                state.connection.mode == ConnectionMode.CONNECTING ->
                    OutlinedButton(
                        onClick = onDisconnect,
                        modifier = Modifier.fillMaxWidth().testTag("connection-disconnect"),
                    ) {
                        Icon(Icons.Filled.LinkOff, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Disconnect")
                    }
                state.connection.connected -> {
                    if (state.connection.mode == ConnectionMode.REMOTE_BACKEND) {
                        Button(
                            onClick = onConnectNearby,
                            modifier = Modifier.fillMaxWidth().testTag("connection-connect-bluetooth"),
                        ) {
                            Icon(Icons.Filled.Bluetooth, contentDescription = null)
                            Spacer(Modifier.size(8.dp))
                            Text("Switch to Bluetooth")
                        }
                    } else if (remoteSwitchReady) {
                        Button(
                            onClick = onConnectRemote,
                            modifier = Modifier.fillMaxWidth().testTag("connection-connect-wifi"),
                        ) {
                            Icon(Icons.Filled.Wifi, contentDescription = null)
                            Spacer(Modifier.size(8.dp))
                            Text("Switch to Wi-Fi gateway")
                        }
                    }
                    OutlinedButton(
                        onClick = onDisconnect,
                        modifier = Modifier.fillMaxWidth().testTag("connection-disconnect"),
                    ) {
                        Icon(Icons.Filled.LinkOff, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Disconnect")
                    }
                }
                else -> {
                    Button(
                        onClick = when {
                            state.connection.mode == ConnectionMode.PERMISSION_REQUIRED ->
                                onRequestBlePermissions
                            state.connection.detail == "bluetooth_disabled" -> onEnableBluetooth
                            else -> onConnectNearby
                        },
                        modifier = Modifier.fillMaxWidth().testTag("connection-connect"),
                    ) {
                        Icon(Icons.Filled.Bluetooth, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text(
                            when {
                                state.connection.mode == ConnectionMode.PERMISSION_REQUIRED ->
                                    "Allow Bluetooth and connect"
                                state.connection.detail == "bluetooth_disabled" ->
                                    "Turn on Bluetooth"
                                else -> "Connect via Bluetooth"
                            },
                        )
                    }
                    if (remoteSignedIn) {
                        OutlinedButton(
                            onClick = onConnectRemote,
                            modifier = Modifier.fillMaxWidth().testTag("connection-connect-wifi"),
                        ) {
                            Icon(Icons.Filled.Wifi, contentDescription = null)
                            Spacer(Modifier.size(8.dp))
                            Text("Connect via Wi-Fi gateway")
                        }
                    }
                    if (state.connection.mode == ConnectionMode.PERMISSION_REQUIRED) {
                        OutlinedButton(
                            onClick = onOpenAppSettings,
                            modifier = Modifier.fillMaxWidth().testTag("connection-open-app-settings"),
                        ) {
                            Icon(Icons.Filled.Settings, contentDescription = null)
                            Spacer(Modifier.size(8.dp))
                            Text("Open Android app settings")
                        }
                    }
                }
            }
        }
    }
}

internal fun connectionPresentation(state: OwnerState): ConnectionPresentation = when {
    state.pairing -> ConnectionPresentation(
        "Secure pairing",
        "Remote access is disabled while this phone is pairing.",
        Color(0xFF1E6552),
        Icons.Filled.Lock,
    )
    state.connection.mode == ConnectionMode.CONNECTING -> {
        val remoteProgress = state.connection.detail in setOf(
            "bonded_kitsu_absent_checking_backend",
            "checking_authenticated_remote_service",
            "enrollment_waiting_for_backend",
            "enrollment_waiting_for_authenticated_backend",
        )
        ConnectionPresentation(
            if (remoteProgress) "Connecting via Wi-Fi" else "Finding Kitsu",
            when (state.connection.detail) {
                "scanning_bonded_kitsu" ->
                    "Scanning for your paired Kitsu over Bluetooth."
                "bonded_kitsu_absent_checking_backend" ->
                    "No bonded Kitsu nearby; checking the authenticated remote service."
                "checking_authenticated_remote_service" ->
                    "Contacting the authenticated owner service and selected gateway companion."
                "enrollment_waiting_for_backend" ->
                    "Waiting for the newly enrolled Kitsu to appear online."
                "enrollment_waiting_for_authenticated_backend" ->
                    "Verifying the Kitsu and gateway on the authenticated remote path."
                else -> "Starting the requested connection."
            },
            Color(0xFF42536F),
            if (remoteProgress) Icons.Filled.Wifi else Icons.Filled.Sync,
        )
    }
    state.connection.mode == ConnectionMode.DIRECT_BLE -> ConnectionPresentation(
        "Connected nearby",
        "Encrypted direct Bluetooth session.",
        Color(0xFF1E6552),
        Icons.Filled.BluetoothConnected,
    )
    state.connection.mode == ConnectionMode.REMOTE_BACKEND -> ConnectionPresentation(
        "Connected remotely",
        when (state.connection.detail) {
            "owner_selected_remote_service" ->
                "Using the authenticated Wi-Fi gateway connection you selected."
            "enrollment_completed", "enrollment_authenticated_remote_path" ->
                "Kitsu and its Wi-Fi gateway were verified after enrollment."
            else -> "Kitsu was not nearby; using its authenticated Wi-Fi gateway."
        },
        Color(0xFF5B477A),
        Icons.Filled.Cloud,
    )
    state.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> ConnectionPresentation(
        "Bluetooth permission needed",
        "Allow Nearby devices so Kitsu can scan and connect.",
        Color(0xFF745019),
        Icons.Filled.Bluetooth,
    )
    state.connection.detail == "bluetooth_disabled" -> ConnectionPresentation(
        "Bluetooth is off",
        "Turn on Bluetooth, then the app will retry the nearby connection.",
        Color(0xFF745019),
        Icons.Filled.Bluetooth,
    )
    state.connection.detail == "user_disconnected" -> ConnectionPresentation(
        "Disconnected",
        "Choose Bluetooth or the Wi-Fi gateway when you want to reconnect.",
        Color(0xFF453A36),
        Icons.Filled.LinkOff,
    )
    state.connection.explicitRemoteAttempt -> ConnectionPresentation(
        "Remote connection failed",
        friendlyCode(state.connection.detail),
        Color(0xFF683737),
        Icons.Filled.CloudOff,
    )
    state.connection.detail == "sign_in_required" -> ConnectionPresentation(
        "No nearby Kitsu",
        "Connect over Bluetooth, or sign in under More to use the Wi-Fi gateway.",
        Color(0xFF554239),
        Icons.Filled.Bluetooth,
    )
    state.connection.detail in setOf("not_connected", "disconnected") -> ConnectionPresentation(
        "Ready to connect",
        "Choose nearby Bluetooth or the authenticated Wi-Fi gateway.",
        Color(0xFF453A36),
        Icons.Filled.LinkOff,
    )
    else -> ConnectionPresentation(
        "Offline",
        friendlyCode(state.connection.detail),
        Color(0xFF683737),
        Icons.Filled.CloudOff,
    )
}

@Composable
private fun HomeScreen(state: OwnerState, refresh: () -> Unit) {
    LazyColumn(
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        modifier = Modifier.fillMaxSize(),
    ) {
        val status = state.status
        if (status == null) {
            item {
                EmptyCard(if (state.loading) "Loading companion…" else "Connect to load your companion")
            }
        } else {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(
                        status.displayName,
                        style = MaterialTheme.typography.headlineSmall,
                        fontWeight = FontWeight.Bold,
                    )
                    Text(
                        status.companionName ?: "No companion pack installed",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            item {
                MetricCard(
                    title = "Companion",
                    body = "Mood ${status.mood}",
                    footnote = "Battery ${status.batteryPercent?.let { "$it%" } ?: "unknown"}",
                    icon = Icons.Filled.Pets,
                )
            }
            item {
                NeedCard("Energy", status.needs.energy)
            }
            item {
                NeedCard("Curiosity", status.needs.curiosity)
            }
            item {
                NeedCard("Affection", status.needs.affection)
            }
            item {
                MetricCard(
                    title = "Mesh radio",
                    body = "Receive ${ready(status.mesh.rxReady)} • Transmit ${ready(status.mesh.txReady)}",
                    footnote = if (status.mesh.enabled) "Mesh enabled" else "Mesh disabled",
                    icon = Icons.Filled.Hub,
                )
            }
            item {
                Text(
                    "Updated ${formatTime(status.updatedAt)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        state.errorCode?.let { code ->
            item { ErrorCard(friendlyCode(code)) }
        }
        item {
            OutlinedButton(
                onClick = refresh,
                enabled = state.connection.connected && !state.loading,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Filled.Refresh, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text("Refresh")
            }
        }
    }
}

@Composable
private fun NeedCard(label: String, value: Int) {
    Card(Modifier.fillMaxWidth()) {
        Column(
            Modifier.padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text(label, fontWeight = FontWeight.SemiBold)
                Text("${value.coerceIn(0, 100)}%")
            }
            LinearProgressIndicator(
                progress = { value.coerceIn(0, 100) / 100f },
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

@Composable
private fun NetworkScreen(state: OwnerState, viewModel: MainViewModel) {
    var confirmMeshEnable by rememberSaveable { mutableStateOf(false) }
    val direct = state.connection.connected && state.connection.mode == ConnectionMode.DIRECT_BLE
    if (confirmMeshEnable) {
        AlertDialog(
            onDismissRequest = { confirmMeshEnable = false },
            icon = { Icon(Icons.Filled.Campaign, contentDescription = null) },
            title = { Text("Enable Mesh radio?") },
            text = {
                Text(
                    "Kitsu will use UK/EU Narrow at 869.618 MHz and up to 22 dBm. " +
                        "Transmissions remain packet-scoped, but enabling Mesh permits Kitsu to receive and send approved packets.",
                )
            },
            confirmButton = {
                Button(
                    onClick = {
                        confirmMeshEnable = false
                        viewModel.configureMesh(true)
                    },
                ) { Text("Enable Mesh") }
            },
            dismissButton = {
                TextButton(onClick = { confirmMeshEnable = false }) { Text("Cancel") }
            },
        )
    }
    LazyColumn(
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        modifier = Modifier.fillMaxSize(),
    ) {
        item {
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Row(
                        Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        Icon(Icons.Filled.Hub, contentDescription = null)
                        Column(Modifier.weight(1f)) {
                            Text("Mesh radio", fontWeight = FontWeight.SemiBold)
                            Text(
                                "UK/EU Narrow · 869.618 MHz · 22 dBm",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Switch(
                            checked = state.status?.mesh?.enabled == true,
                            onCheckedChange = { enabled ->
                                if (enabled) confirmMeshEnable = true else viewModel.configureMesh(false)
                            },
                            enabled = direct && !state.meshConfigurationInFlight,
                            modifier = Modifier.testTag("mesh-enabled-switch"),
                        )
                    }
                    Text(
                        when {
                            state.meshConfigurationInFlight -> "Applying the persisted radio setting…"
                            !direct -> "Connect directly over Bluetooth to change this device setting."
                            state.status?.mesh?.enabled == true ->
                                "Mesh stays active on Kitsu after the phone disconnects. Queued radio work is not cancelled."
                            else -> "Mesh receive and transmit are disabled on Kitsu."
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        item { HorizontalDivider(Modifier.padding(vertical = 4.dp)) }
        item { SectionHeading("Known mesh contacts", "Contacts and routes reported by Kitsu") }
        if (state.peers.isEmpty()) {
            item { EmptyCard("No known contacts yet") }
        } else {
            items(state.peers, key = { it.id }) { peer ->
                MetricCard(
                    title = peer.name,
                    body = "${peer.role.replaceFirstChar(Char::uppercase)} • ${peer.route ?: "Route unknown"}",
                    footnote = buildString {
                        append(MessageComposerPolicy.compactReference(peer.id))
                        peer.lastHeardAt?.let { append(" • Heard ${formatTime(it)}") }
                    },
                    icon = Icons.Filled.Hub,
                )
            }
        }
        item { HorizontalDivider(Modifier.padding(vertical = 6.dp)) }
        item { SectionHeading("Activity", "Recent retained companion and mesh events") }
        if (state.history.isEmpty()) {
            item { EmptyCard("No retained activity") }
        } else {
            items(state.history.asReversed(), key = { it.id }) { entry ->
                MetricCard(
                    title = entry.kind.replace('_', ' ').replaceFirstChar(Char::uppercase),
                    body = entry.summary,
                    footnote = formatTime(entry.occurredAt),
                    icon = Icons.Filled.Info,
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MessagesScreen(
    state: OwnerState,
    sendState: MainViewModel.MessageSendState,
    viewModel: MainViewModel,
) {
    var route by rememberSaveable { mutableStateOf(MessageRoute.DIRECT) }
    var target by rememberSaveable { mutableStateOf("") }
    var draft by rememberSaveable { mutableStateOf("") }
    var messageBodyFocused by remember { mutableStateOf(false) }
    val density = LocalDensity.current
    val imeVisible = WindowInsets.ime.getBottom(density) > 0
    val compactComposer = imeVisible && messageBodyFocused
    val composerScroll = rememberScrollState()
    val recipients = if (route == MessageRoute.DIRECT) {
        MessageComposerPolicy.contactRecipients(state.peers)
    } else {
        MessageComposerPolicy.channelRecipients(state.channels)
    }
    val validationError = MessageComposerPolicy.validationError(route, target, draft)
        ?: if (route == MessageRoute.CHANNEL &&
            state.connection.mode == ConnectionMode.DIRECT_BLE &&
            recipients.none { it.reference == target.trim() }
        ) {
            "That channel slot is not configured on Kitsu"
        } else {
            null
        }
    val messagingEnabled = state.connection.connected && when (state.connection.mode) {
        ConnectionMode.DIRECT_BLE -> state.status?.mesh?.oneShotReady == true
        ConnectionMode.REMOTE_BACKEND -> state.status?.let {
            RemoteSnapshotPolicy.validationError(it) == null
        } == true
        else -> false
    }
    val sending = sendState is MainViewModel.MessageSendState.Sending

    LaunchedEffect(sendState) {
        if (sendState is MainViewModel.MessageSendState.Accepted) {
            draft = ""
        }
    }

    LaunchedEffect(compactComposer) {
        if (compactComposer) {
            // The full composer can be scrolled to the body before the IME opens. Once
            // its leading controls collapse, that retained offset would otherwise keep
            // the compact recipient summary just above the visible viewport.
            composerScroll.scrollTo(0)
        }
    }

    Column(Modifier.fillMaxSize()) {
        if (!imeVisible) {
            if (state.messages.isEmpty()) {
                Box(Modifier.weight(1f).fillMaxWidth().padding(16.dp), contentAlignment = Alignment.Center) {
                    EmptyCard("No messages yet. Choose a contact or channel below to start.")
                }
            } else {
                LazyColumn(
                    modifier = Modifier.weight(1f).fillMaxWidth(),
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    items(state.messages.sortedBy { it.occurredAt }, key = { it.id }) { message ->
                        MessageBubble(message)
                    }
                }
            }
        }

        val composerModifier = if (imeVisible) {
            Modifier.fillMaxWidth().weight(1f)
        } else {
            Modifier.fillMaxWidth().heightIn(max = 340.dp)
        }
        Surface(
            modifier = composerModifier.testTag("message-composer"),
            tonalElevation = 6.dp,
            shadowElevation = 4.dp,
        ) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .imePadding()
                    .verticalScroll(composerScroll)
                    .padding(horizontal = 12.dp, vertical = 10.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (!compactComposer) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(
                            selected = route == MessageRoute.DIRECT,
                            onClick = {
                                route = MessageRoute.DIRECT
                                target = ""
                            },
                            label = { Text("Contact") },
                            leadingIcon = { Icon(Icons.Filled.Hub, contentDescription = null) },
                        )
                        FilterChip(
                            selected = route == MessageRoute.CHANNEL,
                            onClick = {
                                route = MessageRoute.CHANNEL
                                target = MessageComposerPolicy.channelRecipients(state.channels)
                                    .firstOrNull()?.reference.orEmpty()
                            },
                            label = { Text("Channel") },
                            leadingIcon = { Icon(Icons.Filled.Campaign, contentDescription = null) },
                        )
                    }
                    if (recipients.isNotEmpty()) {
                        LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            items(recipients, key = { it.reference }) { recipient ->
                                AssistChip(
                                    onClick = { target = recipient.reference },
                                    label = {
                                        Text(
                                            recipient.label,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis,
                                        )
                                    },
                                    leadingIcon = if (target == recipient.reference) {
                                        { Icon(Icons.Filled.Check, contentDescription = null) }
                                    } else {
                                        null
                                    },
                                )
                            }
                        }
                    }
                    OutlinedTextField(
                        value = target,
                        onValueChange = { target = it.trimStart() },
                        label = {
                            Text(
                                if (route == MessageRoute.DIRECT) {
                                    "Contact public-key reference"
                                } else {
                                    "Channel slot or reference"
                                },
                            )
                        },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth().testTag("message-target"),
                    )
                } else {
                    Text(
                        "To: ${target.ifBlank { "not selected" }}",
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = MaterialTheme.typography.labelMedium,
                        modifier = Modifier.testTag("message-compact-target"),
                    )
                }
                OutlinedTextField(
                    value = draft,
                    onValueChange = { draft = MessageComposerPolicy.acceptText(draft, it) },
                    label = { Text("Message") },
                    supportingText = {
                        Column {
                            Text("${MessageComposerPolicy.utf8Bytes(draft)} / $MAX_MESSAGE_UTF8_BYTES UTF-8 bytes")
                            if (route == MessageRoute.CHANNEL &&
                                state.connection.mode == ConnectionMode.REMOTE_BACKEND &&
                                recipients.none { it.reference == target.trim() }
                            ) {
                                Text("Channel metadata is unknown; Kitsu will validate slot ${target.trim()}.")
                            }
                        }
                    },
                    trailingIcon = {
                        IconButton(
                            onClick = { viewModel.sendMessage(target, draft, route) },
                            enabled = messagingEnabled && validationError == null && !sending,
                            modifier = Modifier.testTag("message-send"),
                        ) {
                            if (sending) {
                                CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                            } else {
                                Icon(Icons.AutoMirrored.Filled.Send, contentDescription = "Send message")
                            }
                        }
                    },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                    keyboardActions = KeyboardActions(
                        onSend = {
                            if (messagingEnabled && validationError == null && !sending) {
                                viewModel.sendMessage(target, draft, route)
                            }
                        },
                    ),
                    minLines = 1,
                    maxLines = 4,
                    modifier = Modifier
                        .fillMaxWidth()
                        .onFocusChanged { messageBodyFocused = it.isFocused }
                        .testTag("message-body"),
                )
                when {
                    !messagingEnabled -> Text(
                        when {
                            !state.connection.connected -> "Connect to Kitsu before sending."
                            state.connection.mode == ConnectionMode.DIRECT_BLE && state.status?.mesh?.enabled != true ->
                                "Enable Mesh from the Mesh screen before sending."
                            state.connection.mode == ConnectionMode.DIRECT_BLE && state.status?.mesh?.timeValid != true ->
                                "Kitsu’s clock is not synchronized yet. Refresh or reconnect."
                            else -> "One-shot Mesh transmit is not ready."
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    sendState is MainViewModel.MessageSendState.Failed -> Text(
                        friendlyCode(sendState.code),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                    sendState is MainViewModel.MessageSendState.Accepted -> Text(
                        "Accepted by Kitsu. Delivery is shown only after the message list refreshes.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.tertiary,
                    )
                    validationError != null && (target.isNotEmpty() || draft.isNotEmpty()) -> Text(
                        validationError,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun MessageBubble(message: Message) {
    val outbound = message.direction.equals("outbound", ignoreCase = true)
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = if (outbound) Arrangement.End else Arrangement.Start,
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(0.86f),
            shape = RoundedCornerShape(
                topStart = 18.dp,
                topEnd = 18.dp,
                bottomStart = if (outbound) 18.dp else 4.dp,
                bottomEnd = if (outbound) 4.dp else 18.dp,
            ),
            colors = CardDefaults.cardColors(
                containerColor = if (outbound) {
                    MaterialTheme.colorScheme.primaryContainer
                } else {
                    MaterialTheme.colorScheme.surfaceVariant
                },
            ),
        ) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Text(
                    if (outbound) {
                        "To ${message.peerId ?: message.channel ?: "mesh"}"
                    } else {
                        "From ${message.peerId ?: message.channel ?: "mesh"}"
                    },
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(message.text)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        formatTime(message.occurredAt),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.size(6.dp))
                    MessageStateIndicator(message.state)
                }
            }
        }
    }
}

@Composable
private fun MessageStateIndicator(state: String) {
    val delivery = MessageComposerPolicy.delivery(state)
    val icon = when (delivery) {
        MessageDelivery.QUEUED -> Icons.Filled.Schedule
        MessageDelivery.SENT -> Icons.Filled.Check
        MessageDelivery.DELIVERED -> Icons.Filled.DoneAll
        MessageDelivery.FAILED -> Icons.Filled.ErrorOutline
        MessageDelivery.UNREAD -> Icons.Filled.MarkEmailUnread
        MessageDelivery.RECEIVED -> Icons.Filled.Check
        MessageDelivery.UNKNOWN -> Icons.Filled.Info
    }
    val label = when (delivery) {
        MessageDelivery.QUEUED -> "Queued"
        MessageDelivery.SENT -> "Sent"
        MessageDelivery.DELIVERED -> "Delivered"
        MessageDelivery.FAILED -> "Failed"
        MessageDelivery.UNREAD -> "Unread"
        MessageDelivery.RECEIVED -> "Received"
        MessageDelivery.UNKNOWN -> state.ifBlank { "Unknown" }.replace('_', ' ')
    }
    val tint = if (delivery == MessageDelivery.FAILED) {
        MaterialTheme.colorScheme.error
    } else {
        MaterialTheme.colorScheme.onSurfaceVariant
    }
    Icon(icon, contentDescription = label, tint = tint, modifier = Modifier.size(16.dp))
    Spacer(Modifier.size(3.dp))
    Text(label, style = MaterialTheme.typography.labelSmall, color = tint)
}

@Composable
private fun CareScreen(state: OwnerState, viewModel: MainViewModel) {
    val connected = state.connection.connected
    val remote = state.connection.mode == ConnectionMode.REMOTE_BACKEND
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            SectionHeading(
                "Care",
                "Every action is authenticated, bounded, replay-safe, and sent only once.",
            )
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                ActionButton(
                    label = "Pet",
                    icon = Icons.Filled.Pets,
                    enabled = connected,
                    modifier = Modifier.weight(1f),
                    onClick = { viewModel.simpleAction(ActionKind.PET) },
                )
                ActionButton(
                    label = "Feed",
                    icon = Icons.Filled.Restaurant,
                    enabled = connected,
                    modifier = Modifier.weight(1f),
                    onClick = { viewModel.simpleAction(ActionKind.FEED) },
                )
            }
        }
        item {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                ActionButton(
                    label = "Play",
                    icon = Icons.Filled.SportsEsports,
                    enabled = connected,
                    modifier = Modifier.weight(1f),
                    onClick = { viewModel.simpleAction(ActionKind.PLAY) },
                )
                ActionButton(
                    label = "Listen",
                    icon = Icons.Filled.Hearing,
                    enabled = connected,
                    modifier = Modifier.weight(1f),
                    onClick = { viewModel.simpleAction(ActionKind.LISTEN_ONCE) },
                )
            }
        }
        item { HorizontalDivider(Modifier.padding(vertical = 4.dp)) }
        item {
            SectionHeading(
                "Mesh presence",
                "Transmit a single introduction. There is no persistent transmit unlock.",
            )
        }
        item {
            Button(
                enabled = remote,
                onClick = { viewModel.simpleAction(ActionKind.ADVERTISE_ONCE) },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Filled.Campaign, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text("Advertise once")
            }
        }
        if (!remote) {
            item {
                EmptyCard("One-shot advertising remains available only through the owner service on this firmware path.")
            }
        }
    }
}

@Composable
private fun ActionButton(
    label: String,
    icon: ImageVector,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Button(onClick = onClick, enabled = enabled, modifier = modifier.height(56.dp)) {
        Icon(icon, contentDescription = null)
        Spacer(Modifier.size(6.dp))
        Text(label, maxLines = 1)
    }
}

@Composable
private fun SettingsScreen(
    state: OwnerState,
    viewModel: MainViewModel,
    requestPermissions: () -> Unit,
    openAppSettings: () -> Unit,
    enableBluetooth: () -> Unit,
    pairController: (String) -> Unit,
    signIn: () -> Unit,
    signOut: () -> Unit,
    ownerAccountStatus: OwnerAccountStatus,
    wifiProvisioningState: MainViewModel.ProvisioningState,
    wifiRetryState: MainViewModel.WifiRetryState,
    gatewayProvisioningState: MainViewModel.ProvisioningState,
    gatewayEnrollmentState: MainViewModel.GatewayEnrollmentState,
    mobileRelayState: MobileRelayUiState,
) {
    var controllerLabel by rememberSaveable { mutableStateOf("") }
    val enrollmentInFlight = gatewayEnrollmentInFlight(gatewayEnrollmentState)
    val enrollmentMonitoring = gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.SwitchingToWifi ||
        gatewayEnrollmentState == MainViewModel.GatewayEnrollmentState.PollingBackend
    val deviceSetupInFlight = enrollmentInFlight ||
        wifiProvisioningState == MainViewModel.ProvisioningState.Saving ||
        wifiRetryState == MainViewModel.WifiRetryState.Retrying ||
        gatewayProvisioningState == MainViewModel.ProvisioningState.Saving
    val pairingBlocked = deviceSetupInFlight || mobileRelayState.enabled ||
        state.connection.mode == ConnectionMode.CONNECTING || state.loading
    val uriHandler = LocalUriHandler.current
    var showOwnerAccount by rememberSaveable { mutableStateOf(false) }
    if (showOwnerAccount) {
        OwnerAccountScreen(
            state = state,
            ownerAccountStatus = ownerAccountStatus,
            onBack = { showOwnerAccount = false },
            onSignIn = signIn,
            onSignOut = signOut,
            onLoadRemoteCompanions = viewModel::refreshRemoteCompanions,
            onSelectRemoteCompanion = viewModel::selectRemoteCompanion,
            onConnectRemote = viewModel::reconnectRemote,
            onOpenGuide = { uriHandler.openUri("https://docs.k32.run/android/#owner-account") },
        )
        return
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text("Kitsu owner app", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Settings for nearby pairing, Wi-Fi, and optional remote access.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        item {
            SectionHeading(
                "Owner account",
                "Optional remote access for a companion connected through its Wi-Fi gateway.",
            )
        }
        item {
            val presentation = OwnerAccountUiPolicy.presentation(ownerAccountStatus)
            OutlinedCard(Modifier.fillMaxWidth().testTag("owner-account-summary")) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        presentation.statusLabel,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.testTag("owner-account-status"),
                    )
                    Text(
                        "Nearby Bluetooth does not need an account. Owner access is only for remote connections.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Button(
                        onClick = { showOwnerAccount = true },
                        modifier = Modifier.fillMaxWidth().testTag("owner-account-open"),
                    ) {
                        Icon(Icons.Filled.Cloud, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Manage owner access")
                    }
                }
            }
        }
        item {
            OutlinedCard(Modifier.fillMaxWidth().testTag("mobile-relay-settings")) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("Public gateway", style = MaterialTheme.typography.titleMedium)
                        Text(
                            when {
                                mobileRelayState.pairedDeviceCount == 0 ->
                                    "Pair a Kitsu first. Existing Wi-Fi and LAN connectivity is unchanged."
                                mobileRelayState.detail == "hold_prg_to_connect" -> {
                                    val seconds = mobileRelayState.enrollmentRemainingMillis
                                        ?.let { (it + 999) / 1_000 }
                                    "Hold PRG on Kitsu to connect" +
                                        (seconds?.let { " · ${it}s remaining" } ?: "")
                                }
                                mobileRelayState.detail == "connected_public_gateway" ->
                                    "Connected to public gateway"
                                mobileRelayState.detail == "finishing_public_gateway" ->
                                    "Finishing public gateway connection"
                                mobileRelayState.enabled ->
                                    "Connecting to public gateway · ${friendlyCode(mobileRelayState.detail)}"
                                else ->
                                    "Connect up to three paired Kitsu devices without an owner account."
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    if (mobileRelayState.enabled) {
                        OutlinedButton(
                            onClick = { viewModel.setMobileRelayEnabled(false) },
                            modifier = Modifier.fillMaxWidth().testTag("mobile-relay-toggle"),
                        ) {
                            Text("Disconnect public gateway")
                        }
                    } else {
                        Button(
                            onClick = { viewModel.setMobileRelayEnabled(true) },
                            enabled = mobileRelayState.pairedDeviceCount > 0 && !pairingBlocked,
                            modifier = Modifier.fillMaxWidth().testTag("mobile-relay-toggle"),
                        ) {
                            Text("Connect to public gateway")
                        }
                    }
                }
            }
        }
        item { HorizontalDivider(Modifier.padding(vertical = 4.dp)) }
        item { SectionHeading("Connection", "Choose the connection you want to use now.") }
        if (state.connection.mode == ConnectionMode.PERMISSION_REQUIRED) {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = requestPermissions,
                        enabled = !state.pairing && !enrollmentInFlight,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Icon(Icons.Filled.Bluetooth, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Allow Bluetooth")
                    }
                    OutlinedButton(
                        onClick = openAppSettings,
                        enabled = !state.pairing && !enrollmentInFlight,
                        modifier = Modifier.fillMaxWidth().testTag("settings-open-app-settings"),
                    ) {
                        Icon(Icons.Filled.Settings, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Open Android app settings")
                    }
                }
            }
        }
        item {
            when {
                enrollmentInFlight -> {
                    OutlinedButton(
                        onClick = viewModel::disconnect,
                        enabled = !state.pairing,
                        modifier = Modifier.fillMaxWidth().testTag("settings-stop-setup"),
                    ) {
                        Icon(Icons.Filled.LinkOff, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text(if (enrollmentMonitoring) "Stop monitoring" else "Stop setup")
                    }
                }
                state.connection.mode == ConnectionMode.CONNECTING -> {
                    OutlinedButton(
                        onClick = viewModel::disconnect,
                        enabled = !state.pairing,
                        modifier = Modifier.fillMaxWidth().testTag("settings-disconnect"),
                    ) {
                        Icon(Icons.Filled.LinkOff, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Cancel connection")
                    }
                }
                state.connection.mode == ConnectionMode.REMOTE_BACKEND -> {
                    Button(
                        onClick = viewModel::reconnectBluetooth,
                        enabled = !state.pairing,
                        modifier = Modifier.fillMaxWidth().testTag("settings-connect-bluetooth"),
                    ) {
                        Icon(Icons.Filled.Bluetooth, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Switch to Bluetooth")
                    }
                }
                state.connection.mode == ConnectionMode.DIRECT_BLE &&
                    ownerAccountStatus == OwnerAccountStatus.SIGNED_IN &&
                    wifiRemoteHandoffReady(state) -> {
                    Button(
                        onClick = viewModel::reconnectRemote,
                        enabled = !state.pairing,
                        modifier = Modifier.fillMaxWidth().testTag("settings-connect-wifi"),
                    ) {
                        Icon(Icons.Filled.Wifi, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Switch to Wi-Fi gateway")
                    }
                }
                state.connection.connected ||
                    state.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> Unit
                else -> {
                    Button(
                        onClick = if (state.connection.detail == "bluetooth_disabled") {
                            enableBluetooth
                        } else {
                            viewModel::reconnectBluetooth
                        },
                        enabled = !state.pairing &&
                            state.connection.mode != ConnectionMode.PERMISSION_REQUIRED,
                        modifier = Modifier.fillMaxWidth().testTag("settings-connect-bluetooth"),
                    ) {
                        Icon(Icons.Filled.Bluetooth, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text(
                            if (state.connection.detail == "bluetooth_disabled") {
                                "Turn on Bluetooth"
                            } else {
                                "Connect via Bluetooth"
                            },
                        )
                    }
                }
            }
        }
        if (!enrollmentInFlight &&
            !state.connection.connected &&
            state.connection.mode != ConnectionMode.CONNECTING &&
            ownerAccountStatus == OwnerAccountStatus.SIGNED_IN
        ) {
            item {
                OutlinedButton(
                    onClick = viewModel::reconnectRemote,
                    enabled = !state.pairing,
                    modifier = Modifier.fillMaxWidth().testTag("settings-connect-wifi"),
                ) {
                    Icon(Icons.Filled.Wifi, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Connect via Wi-Fi gateway")
                }
            }
        }
        if (!enrollmentInFlight &&
            state.connection.connected && state.connection.mode != ConnectionMode.CONNECTING
        ) {
            item {
                OutlinedButton(
                    onClick = viewModel::disconnect,
                    enabled = !state.pairing,
                    modifier = Modifier.fillMaxWidth().testTag("settings-disconnect"),
                ) {
                    Icon(Icons.Filled.LinkOff, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Disconnect")
                }
            }
        }
        item { HorizontalDivider(Modifier.padding(vertical = 4.dp)) }
        item {
            SecureProvisioningSettings(
                ownerState = state,
                wifiState = wifiProvisioningState,
                wifiRetryState = wifiRetryState,
                gatewayState = gatewayProvisioningState,
                enrollmentState = gatewayEnrollmentState,
                remoteAccountSignedIn = ownerAccountStatus == OwnerAccountStatus.SIGNED_IN,
                onSaveWifi = viewModel::provisionWifi,
                onRetryWifi = viewModel::retryWifi,
                onSaveGateway = viewModel::configureGateway,
                onConnectRemote = viewModel::reconnectRemote,
                onRefreshGatewayCatalog = viewModel::refreshGatewayCatalog,
                onEnrollGateway = viewModel::enrollGateway,
                onStopEnrollmentBeforeConfirmation =
                    viewModel::stopGatewayEnrollmentBeforeConfirmation,
            )
        }
        item { HorizontalDivider(Modifier.padding(vertical = 4.dp)) }
        item {
            SectionHeading(
                "Pair this phone",
                "On Kitsu, open Pair Phone. Compare Android’s six-digit code with MATCH CODE and hold PRG only when they match. Hold PRG again at PHONE READY.",
            )
        }
        item {
            OutlinedTextField(
                value = controllerLabel,
                onValueChange = {
                    if (it.toByteArray(Charsets.UTF_8).size <= 24) controllerLabel = it
                },
                enabled = !state.pairing && !pairingBlocked,
                label = { Text("Phone label") },
                supportingText = {
                    Text("${controllerLabel.toByteArray(Charsets.UTF_8).size} / 24 UTF-8 bytes")
                },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
        }
        state.pairingProgress?.let { progress ->
            item {
                MetricCard(
                    progress.stage.name.lowercase().replace('_', ' ').replaceFirstChar(Char::uppercase),
                    friendlyCode(progress.detail),
                    icon = Icons.Filled.Lock,
                )
            }
        }
        item {
            if (state.pairing) {
                OutlinedButton(
                    onClick = viewModel::cancelPairing,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Icon(Icons.Filled.LinkOff, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Cancel pairing")
                }
            } else {
                Button(
                    enabled = !pairingBlocked &&
                        controllerLabel.trim().toByteArray(Charsets.UTF_8).size in 1..24,
                    onClick = { pairController(controllerLabel) },
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Icon(Icons.Filled.Lock, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Pair nearby Kitsu")
                }
            }
        }
        if (state.errorCode == "bluetooth_permission_required") {
            item {
                OutlinedButton(
                    onClick = openAppSettings,
                    modifier = Modifier.fillMaxWidth().testTag("pairing-open-app-settings"),
                ) {
                    Icon(Icons.Filled.Settings, contentDescription = null)
                    Spacer(Modifier.size(8.dp))
                    Text("Open Android app settings")
                }
            }
        }
    }
}

@Composable
private fun OwnerAccountScreen(
    state: OwnerState,
    ownerAccountStatus: OwnerAccountStatus,
    onBack: () -> Unit,
    onSignIn: () -> Unit,
    onSignOut: () -> Unit,
    onLoadRemoteCompanions: () -> Unit,
    onSelectRemoteCompanion: (String) -> Unit,
    onConnectRemote: () -> Unit,
    onOpenGuide: () -> Unit,
) {
    val presentation = OwnerAccountUiPolicy.presentation(ownerAccountStatus)
    Column(Modifier.fillMaxSize().testTag("owner-account-screen")) {
        TextButton(
            onClick = onBack,
            modifier = Modifier.padding(horizontal = 8.dp).testTag("owner-account-back"),
        ) {
            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
            Spacer(Modifier.size(8.dp))
            Text("Back to settings")
        }
        LazyColumn(
            modifier = Modifier.weight(1f).fillMaxWidth(),
            contentPadding = PaddingValues(start = 16.dp, top = 4.dp, end = 16.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            item {
                SectionHeading(
                    "Owner access",
                    "Remote companion access that is separate from nearby Bluetooth.",
                )
            }
            item {
                OutlinedCard(Modifier.fillMaxWidth().testTag("owner-account-explainer")) {
                    Column(
                        Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        Text(OwnerAccountUiPolicy.PURPOSE, style = MaterialTheme.typography.bodyMedium)
                        Text(
                            OwnerAccountUiPolicy.BLUETOOTH_BOUNDARY,
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.SemiBold,
                            color = MaterialTheme.colorScheme.tertiary,
                        )
                        HorizontalDivider()
                        Text("How to get the owner account", style = MaterialTheme.typography.titleMedium)
                        Text(OwnerAccountUiPolicy.INITIAL_ACCESS, style = MaterialTheme.typography.bodyMedium)
                        Text(
                            OwnerAccountUiPolicy.RECOVERY,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        TextButton(
                            onClick = onOpenGuide,
                            modifier = Modifier.testTag("owner-account-guide"),
                        ) {
                            Icon(Icons.Filled.Info, contentDescription = null)
                            Spacer(Modifier.size(8.dp))
                            Text("Read the owner account guide")
                        }
                    }
                }
            }
            item {
                OutlinedCard(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Text(
                            presentation.statusLabel,
                            style = MaterialTheme.typography.titleMedium,
                            fontWeight = FontWeight.SemiBold,
                            modifier = Modifier.testTag("owner-account-status-detail"),
                        )
                        Text(
                            presentation.statusDetail,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        when (ownerAccountStatus) {
                            OwnerAccountStatus.CHECKING ->
                                LinearProgressIndicator(Modifier.fillMaxWidth())
                            OwnerAccountStatus.SIGNED_OUT -> Button(
                                onClick = onSignIn,
                                enabled = !state.pairing,
                                modifier = Modifier.fillMaxWidth().testTag("owner-sign-in"),
                            ) { Text("Sign in for remote access") }
                            OwnerAccountStatus.SIGNED_IN -> OutlinedButton(
                                onClick = onSignOut,
                                enabled = !state.pairing,
                                modifier = Modifier.fillMaxWidth().testTag("owner-sign-out"),
                            ) { Text("Sign out of remote access") }
                        }
                    }
                }
            }
            if (ownerAccountStatus == OwnerAccountStatus.SIGNED_IN) {
                item {
                    Button(
                        onClick = onConnectRemote,
                        enabled = !state.pairing && state.connection.mode != ConnectionMode.CONNECTING &&
                            state.connection.mode != ConnectionMode.REMOTE_BACKEND &&
                            (state.connection.mode != ConnectionMode.DIRECT_BLE ||
                                wifiRemoteHandoffReady(state)),
                        modifier = Modifier.fillMaxWidth().testTag("owner-connect-remote"),
                    ) {
                        Icon(Icons.Filled.Cloud, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Connect through owner service")
                    }
                }
                item {
                    Button(
                        onClick = onLoadRemoteCompanions,
                        enabled = !state.pairing,
                        modifier = Modifier.fillMaxWidth().testTag("owner-load-remote"),
                    ) {
                        Icon(Icons.Filled.Cloud, contentDescription = null)
                        Spacer(Modifier.size(8.dp))
                        Text("Load my remote companions")
                    }
                }
                if (state.remoteCompanions.isEmpty()) {
                    item { EmptyCard("No remote companions loaded yet.") }
                }
                items(state.remoteCompanions, key = { it.id }) { companion ->
                    val selected = companion.id == state.selectedRemoteCompanionId
                    OutlinedButton(
                        onClick = { onSelectRemoteCompanion(companion.id) },
                        enabled = !selected && !state.pairing,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Column(Modifier.fillMaxWidth()) {
                            Text(
                                if (selected) "Selected: ${companion.displayName}" else "Use ${companion.displayName}",
                                fontWeight = FontWeight.SemiBold,
                            )
                            Text(
                                companion.id,
                                style = MaterialTheme.typography.labelSmall,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SectionHeading(title: String, supporting: String) {
    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
        Text(
            supporting,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun EmptyCard(text: String) = OutlinedCard(Modifier.fillMaxWidth()) {
    Text(text, modifier = Modifier.padding(16.dp), color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun ErrorCard(text: String) = Card(
    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
    modifier = Modifier.fillMaxWidth(),
) {
    Row(
        Modifier.padding(14.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(Icons.Filled.ErrorOutline, contentDescription = null)
        Text(text, Modifier.weight(1f))
    }
}

@Composable
private fun MetricCard(
    title: String,
    body: String,
    footnote: String? = null,
    icon: ImageVector? = null,
) = Card(Modifier.fillMaxWidth()) {
    Row(
        Modifier.padding(14.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        icon?.let { Icon(it, contentDescription = null, modifier = Modifier.size(22.dp)) }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, fontWeight = FontWeight.SemiBold)
            Text(body)
            footnote?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

private fun ready(value: Boolean): String = if (value) "ready" else "not ready"

private fun gatewayEnrollmentInFlight(state: MainViewModel.GatewayEnrollmentState): Boolean = when (state) {
    MainViewModel.GatewayEnrollmentState.CreatingClaim,
    is MainViewModel.GatewayEnrollmentState.WaitingForPhysicalConfirmation,
    MainViewModel.GatewayEnrollmentState.SwitchingToWifi,
    MainViewModel.GatewayEnrollmentState.PollingBackend -> true
    else -> false
}

private fun friendlyCode(code: String): String = when {
    code in setOf("not_connected", "disconnected", "user_disconnected") ->
        "Not connected."
    code == "bonded_kitsu_absent" ->
        "Your paired Kitsu was not found nearby."
    code == "bluetooth_disabled" ->
        "Bluetooth is turned off."
    code == "bluetooth_unavailable" ->
        "This phone does not provide Bluetooth Low Energy."
    code in setOf("bluetooth_permission_required", "permission_denied") ->
        "Nearby devices permission is required for Bluetooth."
    code == "location_services_disabled" ->
        "Turn on Android Location services to scan on this Android version."
    code in setOf("scanner_unavailable", "scan_start_failed") || code.startsWith("scan_failed_") ->
        "Android could not start the Bluetooth scan. Turn Bluetooth off and on, then retry."
    code == "bond_missing_repair_required" ->
        "Android lost the system bond. Pair this phone with Kitsu again."
    code in setOf(
        "direct_connect_failed",
        "gatt_timeout",
        "service_discovery_start_failed",
        "service_discovery_failed",
        "service_missing",
        "write_characteristic_missing",
        "notify_characteristic_missing",
        "notify_enable_failed",
        "notify_descriptor_missing",
        "notify_descriptor_write_failed",
    ) || code.startsWith("gatt_status_") ->
        "Bluetooth found Kitsu but could not open the app connection. Retry nearby."
    code in setOf("invalid_controller_capability", "controller_auth_failed") ->
        "This phone’s saved Kitsu pairing could not be authenticated. Pair the phone again."
    code == "controller_auth_backoff" ->
        "Kitsu paused authentication after failed attempts. Wait 30 seconds, then retry."
    code == "clock_sync_failed" ->
        "Kitsu connected, but its clock could not be synchronized. Retry nearby."
    code == "sign_in_required" ->
        "Sign in to use the Wi-Fi gateway connection."
    code == "no_remote_companion" ->
        "No Kitsu is enrolled in this owner account yet."
    code == "companion_selection_required" ->
        "Choose which Kitsu to connect under Owner access."
    code == "remote_companion_offline" ->
        "The enrolled Kitsu is not online through its gateway."
    code == "existing_gateway_enrollment_requires_reset" ->
        "This Kitsu is still enrolled to an earlier gateway. Reset its gateway enrollment or contact support."
    code in setOf("backend_unavailable", "backend_permission_invalid") || code.startsWith("http_") ->
        "The authenticated owner service could not be reached. Check internet access and retry."
    code == "connection_cancelled" ->
        "The connection attempt was cancelled."
    else -> code.replace('_', ' ').trim().replaceFirstChar(Char::uppercase)
}

private val timeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")
    .withZone(ZoneId.systemDefault())

private fun formatTime(epochSeconds: Long): String = timeFormatter.format(Instant.ofEpochSecond(epochSeconds))
