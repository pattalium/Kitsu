package ptl.kitsu.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.activity.compose.BackHandler
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Message
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.AutoStories
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Hub
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SystemUpdateAlt
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import ptl.kitsu.app.FirmwareUpdateUiState
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.R
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.ActionKind
import ptl.kitsu.app.automation.KitsuAutomationAction
import ptl.kitsu.app.navigation.AppRoute
import ptl.kitsu.app.navigation.CompanionDestination
import ptl.kitsu.app.navigation.CompanionDestinationPolicy
import ptl.kitsu.app.security.ControllerRole
import ptl.kitsu.app.update.FirmwareInstallStage
import ptl.kitsu.app.update.locksCompanionControls

private enum class OwnerTab(val label: String) {
    HOME("Kitsu"),
    COMPANION("Companion"),
    MESH("Mesh"),
    MESSAGES("Messages"),
    SETTINGS("Settings"),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun KitsuOwnerApp(
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
    onExportModerationReport: (ModerationReport) -> Unit,
    /** False enables alerts; true requests connected-device continuity. */
    onRequestNotificationPermission: (continuity: Boolean) -> Unit,
    onRequestWalkPermission: () -> Unit = {},
    onCopyAutomationToken: (String) -> Unit = {},
) {
    val owner by viewModel.owner.collectAsStateWithLifecycle()
    val notice by viewModel.notice.collectAsStateWithLifecycle()
    val firmware by viewModel.firmware.collectAsStateWithLifecycle()
    val encounterUnlocks by viewModel.encounterUnlocks.collectAsStateWithLifecycle()
    val neighborActionsInFlight by viewModel.neighborActionsInFlight.collectAsStateWithLifecycle()
    val messageSubmissionInFlight by viewModel.messageSubmissionInFlight.collectAsStateWithLifecycle()
    val launchRequest by viewModel.launchRequest.collectAsStateWithLifecycle()
    val walkSteps by viewModel.walkSteps.collectAsStateWithLifecycle()
    val activeRole = owner.savedKitsu.firstOrNull {
        it.deviceAddress.equals(owner.activeDeviceAddress, ignoreCase = true)
    }?.role ?: ControllerRole.OWNER
    val updateBusy = firmware.progress.stage.locksCompanionControls
    var tab by rememberSaveable { mutableStateOf(OwnerTab.HOME) }
    var selectedMessageThreadKey by rememberSaveable { mutableStateOf<String?>(null) }
    var companionDestination by rememberSaveable {
        mutableStateOf(CompanionDestination.OVERVIEW)
    }
    var automationActionName by rememberSaveable { mutableStateOf<String?>(null) }
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current
    val moderationPreferences = remember(context) { ModerationPreferences(context) }
    var acceptedPolicyVersion by remember {
        mutableStateOf(moderationPreferences.acceptedPolicyVersion())
    }
    var blockedPeerIds by remember {
        mutableStateOf(moderationPreferences.blockedPeerIds())
    }

    LaunchedEffect(launchRequest?.id, updateBusy, activeRole) {
        val request = launchRequest ?: return@LaunchedEffect
        if (updateBusy) return@LaunchedEffect
        when (val route = request.route) {
            AppRoute.Home -> {
                selectedMessageThreadKey = null
                automationActionName = null
                tab = OwnerTab.HOME
                viewModel.consumeLaunchRequest(request.id)
            }
            is AppRoute.Companion -> {
                selectedMessageThreadKey = null
                automationActionName = null
                companionDestination = CompanionDestinationPolicy.resolve(
                    activeRole,
                    route.destination,
                )
                if (companionDestination != route.destination) {
                    viewModel.showNotice(
                        "Caretaker access does not include creature ownership records.",
                    )
                }
                tab = OwnerTab.COMPANION
                viewModel.consumeLaunchRequest(request.id)
            }
            is AppRoute.Messages -> {
                if (activeRole == ControllerRole.OWNER) {
                    tab = OwnerTab.MESSAGES
                } else {
                    tab = OwnerTab.HOME
                    viewModel.consumeLaunchRequest(request.id)
                    viewModel.showNotice("Caretaker access does not include MeshCore messages.")
                }
            }
            is AppRoute.Automation -> {
                selectedMessageThreadKey = null
                companionDestination = CompanionDestination.OVERVIEW
                automationActionName = route.action.wireName
                tab = OwnerTab.COMPANION
                viewModel.consumeLaunchRequest(request.id)
            }
        }
    }

    LaunchedEffect(notice) {
        notice?.let {
            snackbar.showSnackbar(it.humanized())
            viewModel.clearNotice()
        }
    }

    LaunchedEffect(activeRole) {
        if (activeRole == ControllerRole.CARETAKER) {
            if (tab == OwnerTab.MESH || tab == OwnerTab.MESSAGES) {
                selectedMessageThreadKey = null
                tab = OwnerTab.HOME
            }
            val resolvedDestination = CompanionDestinationPolicy.resolve(
                activeRole,
                companionDestination,
            )
            if (resolvedDestination != companionDestination) {
                companionDestination = resolvedDestination
                viewModel.showNotice(
                    "Caretaker access does not include creature ownership records.",
                )
            }
        }
    }

    KitsuTheme(themePreference) {
        BackHandler(
            enabled = (selectedMessageThreadKey != null ||
                companionDestination != CompanionDestination.OVERVIEW ||
                automationActionName != null || tab != OwnerTab.HOME) && !updateBusy,
        ) {
            if (tab == OwnerTab.MESSAGES && selectedMessageThreadKey != null) {
                selectedMessageThreadKey = null
            } else if (tab == OwnerTab.COMPANION &&
                (companionDestination != CompanionDestination.OVERVIEW ||
                    automationActionName != null)
            ) {
                automationActionName = null
                companionDestination = CompanionDestination.OVERVIEW
            } else {
                selectedMessageThreadKey = null
                tab = OwnerTab.HOME
            }
        }
        BoxWithConstraints {
            val useNavigationRail = shouldUseNavigationRail(maxWidth.value.toInt())
            val connection = connectionPresentation(owner)
            val messageThreads = MessageThreadPolicy.build(
                owner.messages,
                owner.peers,
                owner.channels,
                blockedPeerIds,
                owner.messageJournalSession,
            )
            val selectedMessageThread = messageThreads.firstOrNull {
                it.key == selectedMessageThreadKey
            }
            val compactMessageDetail = tab == OwnerTab.MESSAGES &&
                selectedMessageThreadKey != null && !useNavigationRail
            val companionSubpage = tab == OwnerTab.COMPANION &&
                (companionDestination != CompanionDestination.OVERVIEW ||
                    automationActionName != null)
            val showSubpageBack = compactMessageDetail || companionSubpage
            val visibleTabs = if (activeRole == ControllerRole.CARETAKER) {
                listOf(OwnerTab.HOME, OwnerTab.COMPANION, OwnerTab.SETTINGS)
            } else {
                OwnerTab.entries
            }
            Scaffold(
                modifier = Modifier
                    .testTag("kitsu-app")
                    .semantics {
                        stateDescription = if (themePreference == KitsuThemePreference.DARK) {
                            "Dark theme"
                        } else {
                            "System theme"
                        }
                    },
                containerColor = MaterialTheme.colorScheme.background,
                topBar = {
                    TopAppBar(
                        modifier = Modifier.testTag("top-app-bar"),
                        navigationIcon = {
                            if (showSubpageBack) {
                                IconButton(
                                    onClick = {
                                        if (compactMessageDetail) selectedMessageThreadKey = null
                                        else {
                                            automationActionName = null
                                            companionDestination = CompanionDestination.OVERVIEW
                                        }
                                    },
                                    enabled = !updateBusy,
                                    modifier = Modifier.testTag(
                                        if (compactMessageDetail) "conversation-back" else "companion-back",
                                    ),
                                ) {
                                    Icon(
                                        Icons.AutoMirrored.Filled.ArrowBack,
                                        contentDescription = if (compactMessageDetail) {
                                            "Back to conversations"
                                        } else {
                                            "Back to Companion"
                                        },
                                    )
                                }
                            }
                        },
                        title = {
                            if (compactMessageDetail) {
                                val fallbackTarget = selectedMessageThreadKey.orEmpty()
                                    .removePrefix("direct:")
                                    .removePrefix("channel:")
                                Column(Modifier.testTag("conversation-title")) {
                                    Text(
                                        selectedMessageThread?.title
                                            ?: MessageComposerPolicy.compactReference(fallbackTarget),
                                        style = MaterialTheme.typography.titleLarge,
                                        maxLines = 1,
                                    )
                                    Text(
                                        selectedMessageThread?.let { thread ->
                                            if (thread.route == MessageRoute.CHANNEL) {
                                                "${thread.subtitle} · sender names are unverified"
                                            } else {
                                                thread.subtitle
                                            }
                                        } ?: "Mesh conversation",
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        maxLines = 1,
                                    )
                                }
                            } else {
                            Row(
                                horizontalArrangement = Arrangement.spacedBy(10.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Icon(
                                    painter = painterResource(R.drawable.kitsu_app_icon_monochrome),
                                    contentDescription = null,
                                    tint = MaterialTheme.colorScheme.primary,
                                    modifier = Modifier.size(28.dp),
                                )
                                Column {
                                    Text("Kitsu", style = MaterialTheme.typography.titleLarge)
                                    Text(
                                        tab.label,
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                            }
                        },
                        actions = {
                            StatusPill(
                                label = connection.label,
                                tone = connection.tone,
                                modifier = Modifier.padding(end = 12.dp),
                            )
                        },
                        colors = TopAppBarDefaults.topAppBarColors(
                            containerColor = MaterialTheme.colorScheme.background,
                            scrolledContainerColor = MaterialTheme.colorScheme.surface,
                        ),
                    )
                },
                snackbarHost = { SnackbarHost(snackbar) },
                bottomBar = {
                    if (!useNavigationRail && !compactMessageDetail) {
                        OwnerNavigationBar(tab, visibleTabs, updateBusy) {
                            selectedMessageThreadKey = null
                            automationActionName = null
                            companionDestination = CompanionDestination.OVERVIEW
                            tab = it
                        }
                    }
                },
            ) { padding ->
                Row(Modifier.fillMaxSize().padding(padding)) {
                    if (useNavigationRail) {
                        OwnerNavigationRail(tab, visibleTabs, updateBusy) {
                            selectedMessageThreadKey = null
                            automationActionName = null
                            companionDestination = CompanionDestination.OVERVIEW
                            tab = it
                        }
                    }
                    Box(
                        Modifier
                            .weight(1f)
                            .fillMaxHeight()
                            .testTag(if (useNavigationRail) "layout-expanded" else "layout-compact"),
                    ) {
                        val screenModifier = Modifier
                            .align(Alignment.TopCenter)
                            .fillMaxHeight()
                            .widthIn(max = 960.dp)
                            .fillMaxWidth()
                        when (tab) {
                OwnerTab.HOME -> KitsuHomeScreen(
                    owner = owner,
                    viewModel = viewModel,
                    updateBusy = updateBusy,
                    neighborActionsInFlight = neighborActionsInFlight,
                    onRequestBlePermissions = onRequestBlePermissions,
                    onEnableBluetooth = onEnableBluetooth,
                    onOpenLocationSettings = onOpenLocationSettings,
                    onOpenAppSettings = onOpenAppSettings,
                    onManageKitsu = { tab = OwnerTab.SETTINGS },
                    canListenNearby = activeRole == ControllerRole.OWNER,
                    modifier = screenModifier,
                )
                OwnerTab.COMPANION -> {
                    val automationAction = KitsuAutomationAction.fromWireName(automationActionName)
                    when {
                        automationAction != null -> {
                            val confirmationTargetAddress = owner.activeDeviceAddress
                            val automationUnavailableReason = when {
                                automationAction != KitsuAutomationAction.STUDIO -> null
                                confirmationTargetAddress == null -> "Select a Kitsu before opening Pet Studio."
                                !owner.connection.connected -> "Connect to this Kitsu before opening Pet Studio."
                                !owner.companionProfileSupported ->
                                    "Update this Kitsu's firmware before opening Pet Studio."
                                else -> null
                            }
                            KitsuAutomationConfirmationScreen(
                                action = automationAction,
                                targetName = owner.companionProfile?.nickname
                                    ?.takeIf(String::isNotBlank)
                                    ?: owner.status?.companionName?.takeIf(String::isNotBlank),
                                targetDeviceId = owner.status?.deviceId,
                                targetAddress = owner.activeDeviceAddress,
                                enabled = !updateBusy && when (automationAction) {
                                    KitsuAutomationAction.WALK,
                                    KitsuAutomationAction.ACCESSIBILITY,
                                    -> true
                                    KitsuAutomationAction.STUDIO -> automationUnavailableReason == null
                                    else -> owner.connection.connected
                                },
                                unavailableReason = automationUnavailableReason,
                                onConfirm = {
                                    automationActionName = null
                                    when (automationAction) {
                                        KitsuAutomationAction.CHECK -> viewModel.refresh()
                                        KitsuAutomationAction.PET -> viewModel.simpleAction(ActionKind.PET)
                                        KitsuAutomationAction.FEED -> viewModel.simpleAction(ActionKind.FEED)
                                        KitsuAutomationAction.PLAY -> viewModel.simpleAction(ActionKind.PLAY)
                                        KitsuAutomationAction.FOCUS_25 -> viewModel.startFocus(25)
                                        KitsuAutomationAction.FOCUS_50 -> viewModel.startFocus(50)
                                        KitsuAutomationAction.WALK ->
                                            companionDestination = CompanionDestination.OVERVIEW
                                        KitsuAutomationAction.ACCESSIBILITY ->
                                            companionDestination = CompanionDestination.ACCESSIBILITY
                                        KitsuAutomationAction.STUDIO -> {
                                            val latestOwner = viewModel.owner.value
                                            val targetStillValid = confirmationTargetAddress != null &&
                                                latestOwner.activeDeviceAddress.equals(
                                                    confirmationTargetAddress,
                                                    ignoreCase = true,
                                                ) && latestOwner.connection.connected &&
                                                latestOwner.companionProfileSupported
                                            if (targetStillValid) {
                                                companionDestination = CompanionDestination.STUDIO
                                            } else {
                                                companionDestination = CompanionDestination.OVERVIEW
                                                viewModel.showNotice(
                                                    "Pet Studio needs a connected Kitsu with current firmware.",
                                                )
                                            }
                                        }
                                    }
                                },
                                onCancel = {
                                    automationActionName = null
                                    companionDestination = CompanionDestination.OVERVIEW
                                },
                                modifier = screenModifier,
                            )
                        }
                        CompanionDestinationPolicy.allows(
                            activeRole,
                            CompanionDestination.GUIDE,
                        ) && companionDestination == CompanionDestination.GUIDE -> KitsuFieldGuideScreen(
                            owner = owner,
                            encounterUnlocks = encounterUnlocks,
                            onSync = viewModel::syncEncounterCodes,
                            modifier = screenModifier,
                        )
                        companionDestination == CompanionDestination.ACCESSIBILITY ->
                            KitsuAccessibilityRoute(
                                status = owner.status,
                                profile = owner.companionProfile,
                                focus = owner.focusState,
                                walk = owner.walkState,
                                presentation = owner.petPresentation,
                                frame = owner.petPresentationFrame,
                                enabled = owner.connection.connected && !updateBusy,
                                captureEnabled = owner.connection.connected && !updateBusy &&
                                    owner.companionProfileSupported &&
                                    !owner.petPresentationInFlight,
                                onRefresh = viewModel::capturePetPresentation,
                                onPet = { viewModel.simpleAction(ActionKind.PET) },
                                onFeed = { viewModel.simpleAction(ActionKind.FEED) },
                                onPlay = { viewModel.simpleAction(ActionKind.PLAY) },
                                onNotice = viewModel::showNotice,
                                modifier = screenModifier.padding(18.dp),
                            )
                        companionDestination == CompanionDestination.STUDIO -> KitsuPetStudioRoute(
                            presentation = owner.petPresentation,
                            frame = owner.petPresentationFrame,
                            enabled = owner.connection.connected &&
                                owner.companionProfileSupported && !updateBusy &&
                                !owner.petPresentationInFlight,
                            onRefreshFrame = viewModel::capturePetPresentation,
                            onNotice = viewModel::showNotice,
                            modifier = screenModifier,
                        )
                        else -> KitsuCompanionScreen(
                            owner = owner,
                            walkSteps = walkSteps,
                            canManageProfile = activeRole == ControllerRole.OWNER,
                            canOpenGuide = CompanionDestinationPolicy.allows(
                                activeRole,
                                CompanionDestination.GUIDE,
                            ),
                            updateBusy = updateBusy,
                            onRefresh = viewModel::refresh,
                            onSetNickname = viewModel::setCompanionNickname,
                            onAnswerRequest = viewModel::answerCompanionRequest,
                            onAnswerQuestion = viewModel::answerCompanionQuestion,
                            onStartFocus = viewModel::startFocus,
                            onStopFocus = viewModel::stopFocus,
                            onCancelFocus = viewModel::cancelFocus,
                            onAcknowledgeFocus = viewModel::acknowledgeFocus,
                            onStartWalk = viewModel::startWalk,
                            onSyncWalk = viewModel::syncWalk,
                            onDecideWalk = viewModel::decideWalk,
                            onFinishWalk = viewModel::finishWalk,
                            onAcknowledgeWalk = viewModel::acknowledgeWalk,
                            onRequestWalkPermission = onRequestWalkPermission,
                            onOpenGuide = {
                                companionDestination = CompanionDestinationPolicy.resolve(
                                    activeRole,
                                    CompanionDestination.GUIDE,
                                )
                            },
                            modifier = screenModifier,
                        )
                    }
                }
                OwnerTab.MESH -> KitsuMeshScreen(
                    owner = owner,
                    viewModel = viewModel,
                    updateBusy = updateBusy,
                    modifier = screenModifier,
                )
                OwnerTab.MESSAGES -> KitsuMessagesScreen(
                    owner = owner,
                    viewModel = viewModel,
                    launchRequest = launchRequest?.takeIf { it.route is AppRoute.Messages },
                    onLaunchRequestConsumed = viewModel::consumeLaunchRequest,
                    selectedThreadKey = selectedMessageThreadKey,
                    onSelectedThreadKeyChange = { selectedMessageThreadKey = it },
                    useMasterDetail = useNavigationRail,
                    updateBusy = updateBusy,
                    messageSubmissionInFlight = messageSubmissionInFlight,
                    policyAccepted = acceptedPolicyVersion == MeshUserPolicy.VERSION,
                    blockedPeerIds = blockedPeerIds,
                    onAcceptPolicy = {
                        moderationPreferences.acceptCurrentPolicy()
                        acceptedPolicyVersion = MeshUserPolicy.VERSION
                    },
                    onBlockPeer = { peerId ->
                        moderationPreferences.blockPeer(peerId)
                        blockedPeerIds = moderationPreferences.blockedPeerIds()
                    },
                    onExportReport = onExportModerationReport,
                    modifier = screenModifier,
                )
                OwnerTab.SETTINGS -> KitsuSettingsScreen(
                    owner = owner,
                    firmware = firmware,
                    encounterUnlocks = encounterUnlocks,
                    viewModel = viewModel,
                    themePreference = themePreference,
                    onThemePreferenceChange = onThemePreferenceChange,
                    onRequestBlePermissions = onRequestBlePermissions,
                    onEnableBluetooth = onEnableBluetooth,
                    onOpenLocationSettings = onOpenLocationSettings,
                    onPairController = onPairController,
                    onPairCaretakerController = onPairCaretakerController,
                    onRetryPairingBlePermissions = onRetryPairingBlePermissions,
                    onFinishPairing = onFinishPairing,
                    onRepairBluetoothPairing = onRepairBluetoothPairing,
                    onOpenBluetoothSettingsForRepair = onOpenBluetoothSettingsForRepair,
                    onOpenFirmwarePackage = onOpenFirmwarePackage,
                    onOpenAppSettings = onOpenAppSettings,
                    onOpenSupportPage = onOpenSupportPage,
                    onOpenUnlockPage = onOpenUnlockPage,
                    onCopyUnlockCode = onCopyUnlockCode,
                    onRequestNotificationPermission = onRequestNotificationPermission,
                    onCopyAutomationToken = onCopyAutomationToken,
                    acceptedPolicyVersion = acceptedPolicyVersion,
                    blockedPeerIds = blockedPeerIds,
                    onAcceptPolicy = {
                        moderationPreferences.acceptCurrentPolicy()
                        acceptedPolicyVersion = MeshUserPolicy.VERSION
                    },
                    onUnblockPeer = { peerId ->
                        moderationPreferences.unblockPeer(peerId)
                        blockedPeerIds = moderationPreferences.blockedPeerIds()
                    },
                    updateBusy = updateBusy,
                    modifier = screenModifier,
                )
                        }
                    }
                }
            }

            if (updateBusy) {
                FirmwareUpdateLockDialog(
                    firmware = firmware,
                    onCancel = viewModel::cancelFirmwareUpdate,
                )
            }
        }
    }
}

@Composable
private fun OwnerNavigationBar(
    selected: OwnerTab,
    tabs: List<OwnerTab>,
    disabled: Boolean,
    onSelect: (OwnerTab) -> Unit,
) {
    NavigationBar(
        containerColor = MaterialTheme.colorScheme.surface,
        modifier = Modifier.testTag("bottom-navigation"),
    ) {
        tabs.forEach { tab ->
            NavigationBarItem(
                selected = selected == tab,
                onClick = { onSelect(tab) },
                icon = { OwnerTabIcon(tab) },
                label = { Text(tab.label) },
                enabled = !disabled,
                modifier = Modifier.testTag(tab.testTag),
            )
        }
    }
}

@Composable
private fun OwnerNavigationRail(
    selected: OwnerTab,
    tabs: List<OwnerTab>,
    disabled: Boolean,
    onSelect: (OwnerTab) -> Unit,
) {
    NavigationRail(
        containerColor = MaterialTheme.colorScheme.surface,
        modifier = Modifier.testTag("navigation-rail"),
    ) {
        Spacer(Modifier.height(12.dp))
        tabs.forEach { tab ->
            NavigationRailItem(
                selected = selected == tab,
                onClick = { onSelect(tab) },
                icon = { OwnerTabIcon(tab) },
                label = { Text(tab.label) },
                enabled = !disabled,
                modifier = Modifier.testTag(tab.testTag),
            )
        }
    }
}

@Composable
private fun OwnerTabIcon(tab: OwnerTab) {
    Icon(
        imageVector = when (tab) {
            OwnerTab.HOME -> Icons.Default.Home
            OwnerTab.COMPANION -> Icons.Default.AutoStories
            OwnerTab.MESH -> Icons.Default.Hub
            OwnerTab.MESSAGES -> Icons.AutoMirrored.Filled.Message
            OwnerTab.SETTINGS -> Icons.Default.Settings
        },
        contentDescription = null,
    )
}

private val OwnerTab.testTag: String
    get() = when (this) {
        OwnerTab.HOME -> "nav-home"
        OwnerTab.COMPANION -> "nav-companion"
        OwnerTab.MESH -> "nav-network"
        OwnerTab.MESSAGES -> "nav-messages"
        OwnerTab.SETTINGS -> "nav-settings"
    }

internal fun shouldUseNavigationRail(widthDp: Int): Boolean = widthDp >= 720

@Composable
private fun FirmwareUpdateLockDialog(
    firmware: FirmwareUpdateUiState,
    onCancel: () -> Unit,
) {
    val stage = firmware.progress.stage
    val canCancel = firmware.updateId != null && stage in setOf(
        FirmwareInstallStage.TRANSFERRING,
        FirmwareInstallStage.VERIFYING,
        FirmwareInstallStage.READY_TO_REBOOT,
    )
    Dialog(
        onDismissRequest = {},
        properties = DialogProperties(
            dismissOnBackPress = false,
            dismissOnClickOutside = false,
        ),
    ) {
        Surface(
            shape = MaterialTheme.shapes.extraLarge,
            tonalElevation = 8.dp,
            modifier = Modifier.fillMaxWidth().testTag("firmware-update-lock"),
        ) {
            Column(
                modifier = Modifier.fillMaxWidth().padding(24.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Icon(
                    Icons.Default.SystemUpdateAlt,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(40.dp),
                )
                Text("Updating your Kitsu", style = MaterialTheme.typography.headlineSmall)
                Text(
                    firmwareStageLabel(stage),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
                if (stage == FirmwareInstallStage.TRANSFERRING) {
                    LinearProgressIndicator(
                        progress = { firmware.progress.percent / 100f },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text("${firmware.progress.percent}% transferred")
                } else {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
                Text(
                    "Keep Kitsu powered and nearby. Navigation and companion controls stay locked until the update finishes or is safely cancelled.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                firmware.progress.firmwareVersion?.let { Text("Firmware $it") }
                Spacer(Modifier.height(2.dp))
                if (canCancel) {
                    TextButton(onClick = onCancel, modifier = Modifier.testTag("firmware-cancel")) {
                        Text("Cancel update safely")
                    }
                } else {
                    Text(
                        "This step cannot be interrupted safely.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

private fun firmwareStageLabel(stage: FirmwareInstallStage): String = when (stage) {
    FirmwareInstallStage.PREPARING -> "Preparing signed package"
    FirmwareInstallStage.TRANSFERRING -> "Sending firmware over Bluetooth"
    FirmwareInstallStage.VERIFYING -> "Verifying on Kitsu"
    FirmwareInstallStage.READY_TO_REBOOT -> "Ready to restart"
    FirmwareInstallStage.REBOOTING -> "Restarting Kitsu"
    else -> stage.name.humanized()
}
