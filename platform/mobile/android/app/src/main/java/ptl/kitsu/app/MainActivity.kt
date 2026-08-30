package ptl.kitsu.app

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ActivityNotFoundException
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.graphics.Color
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PersistableBundle
import android.provider.Settings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import ptl.kitsu.app.ui.KitsuOwnerApp
import ptl.kitsu.app.ui.KitsuThemePreferences
import ptl.kitsu.app.ui.ModerationReport
import ptl.kitsu.app.ui.ModerationReportCodec
import ptl.kitsu.app.update.locksCompanionControls
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.notifications.KitsuNotificationPermissionPolicy
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue

internal const val KITSU_SUPPORT_URL = "https://ko-fi.com/pattalium"
internal const val KITSU_UNLOCK_URL = "https://k32.run/unlock/"
private const val SENSITIVE_CLIPBOARD_EXTRA = "android.content.extra.IS_SENSITIVE"

/** Document providers report no consistent MIME type for `.kitsu-fw`. */
internal fun firmwarePackagePickerMimeTypes(): Array<String> = arrayOf("*/*")

internal fun kitsuSupportIntent(): Intent = Intent(
    Intent.ACTION_VIEW,
    Uri.parse(KITSU_SUPPORT_URL),
).addCategory(Intent.CATEGORY_BROWSABLE)

internal fun kitsuUnlockIntent(code: String): Intent {
    require(EncounterCodePolicy.validCode(code)) { "invalid_encounter_code" }
    val uri = Uri.parse(KITSU_UNLOCK_URL).buildUpon()
        .encodedFragment("code=${Uri.encode(code)}")
        .build()
    return Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE)
}

internal fun kitsuSensitiveUnlockClip(code: String): ClipData {
    require(EncounterCodePolicy.validCode(code)) { "invalid_encounter_code" }
    return ClipData.newPlainText("Kitsu unlock code", code).also { clip ->
        clip.description.extras = PersistableBundle().apply {
            putBoolean(SENSITIVE_CLIPBOARD_EXTRA, true)
        }
    }
}

class MainActivity : ComponentActivity() {
    private val viewModel: MainViewModel by viewModels()

    private enum class BleContinuation {
        CONNECT,
        PAIR_CONTROLLER,
        PAIR_CARETAKER,
        FINISH_PAIRING,
        REPAIR_PAIRING,
    }
    private enum class NotificationContinuation { ALERTS, CONNECTION_CONTINUITY }

    private var permissionContinuation = BleContinuation.CONNECT
    private var bluetoothContinuation = BleContinuation.CONNECT
    private var pendingPairingLabel: String? = null
    private var pendingModerationReport: ModerationReport? = null
    private var notificationContinuation = NotificationContinuation.ALERTS

    private val bluetoothEnableLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        continueWithBluetoothReady(bluetoothContinuation)
    }

    private val locationSettingsLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        viewModel.reconnectBluetooth()
    }

    private val bluetoothSettingsRepairLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        // Android has no supported app API for deleting a stale bond. After the
        // owner returns from the system Forget screen, retry only the dedicated
        // saved-controller repair flow.
        requestBlePermissions(BleContinuation.REPAIR_PAIRING)
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        if (requiredBlePermissions().all { permission ->
                ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
            }
        ) {
            continueAfterPermission(permissionContinuation)
        } else {
            viewModel.reportBlePermissionDenied(
                pairing = permissionContinuation != BleContinuation.CONNECT,
                repair = permissionContinuation == BleContinuation.REPAIR_PAIRING,
            )
        }
    }

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted || KitsuNotificationPermissionPolicy.canPost(this)) {
            continueAfterNotificationPermission(notificationContinuation)
        } else {
            viewModel.showNotice("Notifications remain off. You can allow them later in Android settings.")
        }
    }

    private val walkPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted ->
        if (granted || Build.VERSION.SDK_INT < 29 || ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.ACTIVITY_RECOGNITION,
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            viewModel.startWalkStepTracking()
        } else {
            viewModel.showNotice("Physical activity access is needed for automatic walk steps.")
        }
    }

    private val firmwarePackageLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri -> uri?.let(viewModel::importFirmware) }

    private val moderationReportLauncher = registerForActivityResult(
        ActivityResultContracts.CreateDocument("application/json"),
    ) { uri ->
        val report = pendingModerationReport
        pendingModerationReport = null
        if (uri == null || report == null) return@registerForActivityResult
        val result = runCatching {
            contentResolver.openOutputStream(uri, "wt")?.bufferedWriter(Charsets.UTF_8)?.use {
                it.write(ModerationReportCodec.encode(report))
            } ?: error("report_destination_unavailable")
        }
        if (result.isSuccess) {
            viewModel.showNotice("Report file exported. It was not submitted automatically; share it with Kitsu support for review.")
        } else {
            viewModel.showNotice("Report export failed.")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissionContinuation = savedInstanceState?.getString(STATE_PERMISSION_CONTINUATION)
            ?.let { runCatching { BleContinuation.valueOf(it) }.getOrNull() }
            ?: BleContinuation.CONNECT
        bluetoothContinuation = savedInstanceState?.getString(STATE_BLUETOOTH_CONTINUATION)
            ?.let { runCatching { BleContinuation.valueOf(it) }.getOrNull() }
            ?: BleContinuation.CONNECT
        pendingPairingLabel = savedInstanceState?.getString(STATE_PAIRING_LABEL)
        pendingModerationReport = savedInstanceState?.getString(STATE_MODERATION_REPORT)
            ?.let { runCatching { ModerationReportCodec.decode(it) }.getOrNull() }
        notificationContinuation = savedInstanceState?.getString(STATE_NOTIFICATION_CONTINUATION)
            ?.let { runCatching { NotificationContinuation.valueOf(it) }.getOrNull() }
            ?: NotificationContinuation.ALERTS
        if (savedInstanceState == null) handleLaunchIntent(intent)
        applyEdgeToEdge(dark = true)
        setContent {
            val themePreferences = remember { KitsuThemePreferences(this@MainActivity) }
            var themePreference by remember { mutableStateOf(themePreferences.current()) }
            val effectiveDark = themePreference.useDarkColors(isSystemInDarkTheme())
            SideEffect { applyEdgeToEdge(effectiveDark) }
            val firmware by viewModel.firmware.collectAsStateWithLifecycle()
            val updateActive = firmware.progress.stage.locksCompanionControls
            LaunchedEffect(updateActive) {
                if (updateActive) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            KitsuOwnerApp(
                viewModel = viewModel,
                themePreference = themePreference,
                onThemePreferenceChange = { preference ->
                    themePreferences.set(preference)
                    themePreference = preference
                },
                onRequestBlePermissions = { requestBlePermissions(BleContinuation.CONNECT) },
                onEnableBluetooth = { requestBlePermissions(BleContinuation.CONNECT) },
                onOpenLocationSettings = {
                    locationSettingsLauncher.launch(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
                },
                onPairController = { label ->
                    pendingPairingLabel = label
                    requestBlePermissions(BleContinuation.PAIR_CONTROLLER)
                },
                onPairCaretakerController = { label ->
                    pendingPairingLabel = label
                    requestBlePermissions(BleContinuation.PAIR_CARETAKER)
                },
                onRetryPairingBlePermissions = {
                    requestBlePermissions(permissionContinuation)
                },
                onFinishPairing = {
                    requestBlePermissions(BleContinuation.FINISH_PAIRING)
                },
                onRepairBluetoothPairing = {
                    requestBlePermissions(BleContinuation.REPAIR_PAIRING)
                },
                onOpenBluetoothSettingsForRepair = {
                    bluetoothSettingsRepairLauncher.launch(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                },
                onOpenFirmwarePackage = {
                    // File providers do not agree on a MIME type for `.kitsu-fw`; the strict
                    // signed container reader is the authority after the user selects one URI.
                    firmwarePackageLauncher.launch(firmwarePackagePickerMimeTypes())
                },
                onOpenAppSettings = {
                    startActivity(
                        Intent(
                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.parse("package:$packageName"),
                        ),
                    )
                },
                onOpenSupportPage = {
                    try {
                        startActivity(kitsuSupportIntent())
                    } catch (_: ActivityNotFoundException) {
                        viewModel.showNotice("No web browser is available.")
                    } catch (_: SecurityException) {
                        viewModel.showNotice("The support page could not be opened safely.")
                    }
                },
                onOpenUnlockPage = { code ->
                    try {
                        startActivity(kitsuUnlockIntent(code))
                    } catch (_: ActivityNotFoundException) {
                        viewModel.showNotice("No web browser is available.")
                    } catch (_: IllegalArgumentException) {
                        viewModel.showNotice("This saved unlock code is invalid.")
                    } catch (_: SecurityException) {
                        viewModel.showNotice("The unlock page could not be opened safely.")
                    }
                },
                onCopyUnlockCode = { code ->
                    try {
                        getSystemService(ClipboardManager::class.java)
                            ?.setPrimaryClip(kitsuSensitiveUnlockClip(code))
                        viewModel.showNotice("Unlock code copied.")
                    } catch (_: IllegalArgumentException) {
                        viewModel.showNotice("This saved unlock code is invalid.")
                    }
                },
                onExportModerationReport = { report ->
                    pendingModerationReport = report
                    moderationReportLauncher.launch(report.suggestedFileName())
                },
                onRequestNotificationPermission = { continuity ->
                    requestNotificationPermission(
                        if (continuity) NotificationContinuation.CONNECTION_CONTINUITY
                        else NotificationContinuation.ALERTS,
                    )
                },
                onRequestWalkPermission = {
                    if (Build.VERSION.SDK_INT < 29 || ContextCompat.checkSelfPermission(
                            this@MainActivity,
                            Manifest.permission.ACTIVITY_RECOGNITION,
                        ) == PackageManager.PERMISSION_GRANTED
                    ) {
                        viewModel.startWalkStepTracking()
                    } else {
                        walkPermissionLauncher.launch(Manifest.permission.ACTIVITY_RECOGNITION)
                    }
                },
                onCopyAutomationToken = { token ->
                    val clip = ClipData.newPlainText("Kitsu automation capability", token).also {
                        it.description.extras = PersistableBundle().apply {
                            putBoolean(SENSITIVE_CLIPBOARD_EXTRA, true)
                        }
                    }
                    getSystemService(ClipboardManager::class.java)?.setPrimaryClip(clip)
                    viewModel.showNotice("Tasker setup copied. It includes a private capability.")
                },
            )
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleLaunchIntent(intent)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(STATE_PERMISSION_CONTINUATION, permissionContinuation.name)
        outState.putString(STATE_BLUETOOTH_CONTINUATION, bluetoothContinuation.name)
        pendingPairingLabel?.let { outState.putString(STATE_PAIRING_LABEL, it) }
        pendingModerationReport?.let {
            outState.putString(STATE_MODERATION_REPORT, ModerationReportCodec.encode(it))
        }
        outState.putString(STATE_NOTIFICATION_CONTINUATION, notificationContinuation.name)
        super.onSaveInstanceState(outState)
    }

    private fun requestBlePermissions(continuation: BleContinuation) {
        val permissions = requiredBlePermissions()
        if (permissions.all {
                ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
            }
        ) {
            continueAfterPermission(continuation)
            return
        }
        permissionContinuation = continuation
        permissionLauncher.launch(permissions)
    }

    private fun requestNotificationPermission(continuation: NotificationContinuation) {
        notificationContinuation = continuation
        if (Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.POST_NOTIFICATIONS,
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            continueAfterNotificationPermission(continuation)
        } else {
            notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun continueAfterNotificationPermission(continuation: NotificationContinuation) {
        when (continuation) {
            NotificationContinuation.ALERTS -> viewModel.enableNotificationAlerts()
            NotificationContinuation.CONNECTION_CONTINUITY -> viewModel.enableConnectionContinuity()
        }
    }

    private fun handleLaunchIntent(intent: Intent?) {
        viewModel.handleLaunchIntent(
            action = intent?.action,
            mimeType = intent?.type,
            sharedText = intent?.getCharSequenceExtra(Intent.EXTRA_TEXT),
            routeThreadKey = intent?.getStringExtra(
                ptl.kitsu.app.navigation.AppLaunchIntentPolicy.EXTRA_THREAD_KEY,
            ),
            companionDestination = intent?.getStringExtra(
                ptl.kitsu.app.navigation.AppLaunchIntentPolicy.EXTRA_COMPANION_DESTINATION,
            ),
            automationAction = intent?.getStringExtra(
                ptl.kitsu.app.navigation.AppLaunchIntentPolicy.EXTRA_AUTOMATION_ACTION,
            ),
        )
    }

    private fun requiredBlePermissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private fun continueAfterPermission(continuation: BleContinuation) {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || adapter.isEnabled) {
            continueWithBluetoothReady(continuation)
            return
        }
        bluetoothContinuation = continuation
        bluetoothEnableLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
    }

    private fun continueWithBluetoothReady(continuation: BleContinuation) {
        when (continuation) {
            BleContinuation.CONNECT -> viewModel.reconnectBluetooth()
            BleContinuation.PAIR_CONTROLLER -> {
                pendingPairingLabel?.let(viewModel::pairController)
                pendingPairingLabel = null
            }
            BleContinuation.PAIR_CARETAKER -> {
                pendingPairingLabel?.let(viewModel::pairCaretakerController)
                pendingPairingLabel = null
            }
            BleContinuation.FINISH_PAIRING -> viewModel.finishPendingPairing()
            BleContinuation.REPAIR_PAIRING -> viewModel.repairBluetoothPairing()
        }
    }

    private fun applyEdgeToEdge(dark: Boolean) {
        val navigationScrim = if (dark) DARK_SYSTEM_BAR else LIGHT_SYSTEM_BAR
        enableEdgeToEdge(
            statusBarStyle = if (dark) {
                SystemBarStyle.dark(Color.TRANSPARENT)
            } else {
                SystemBarStyle.light(Color.TRANSPARENT, Color.TRANSPARENT)
            },
            navigationBarStyle = if (dark) {
                SystemBarStyle.dark(navigationScrim)
            } else {
                SystemBarStyle.light(navigationScrim, navigationScrim)
            },
        )
        if (Build.VERSION.SDK_INT >= 29) window.isNavigationBarContrastEnforced = false
    }

    private companion object {
        val DARK_SYSTEM_BAR = 0xFF0B0C0F.toInt()
        val LIGHT_SYSTEM_BAR = 0xFFF8F2E9.toInt()
        const val STATE_PERMISSION_CONTINUATION = "permission_continuation"
        const val STATE_BLUETOOTH_CONTINUATION = "bluetooth_continuation"
        const val STATE_PAIRING_LABEL = "pending_pairing_label"
        const val STATE_MODERATION_REPORT = "pending_moderation_report"
        const val STATE_NOTIFICATION_CONTINUATION = "notification_continuation"
    }
}
