package ptl.kitsu.app

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.graphics.Color
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue

class MainActivity : ComponentActivity() {
    private val viewModel: MainViewModel by viewModels()

    private enum class BleContinuation { CONNECT, PAIR_CONTROLLER, FINISH_PAIRING }

    private var permissionContinuation = BleContinuation.CONNECT
    private var bluetoothContinuation = BleContinuation.CONNECT
    private var pendingPairingLabel: String? = null
    private var pendingModerationReport: ModerationReport? = null

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

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        if (requiredBlePermissions().all { permission ->
                ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
            }
        ) {
            continueAfterPermission(permissionContinuation)
        } else {
            viewModel.reportBlePermissionDenied(permissionContinuation != BleContinuation.CONNECT)
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
                onRetryPairingBlePermissions = {
                    requestBlePermissions(permissionContinuation)
                },
                onFinishPairing = {
                    requestBlePermissions(BleContinuation.FINISH_PAIRING)
                },
                onOpenFirmwarePackage = {
                    // File providers do not agree on a MIME type for `.kitsu-fw`; the strict
                    // signed container reader is the authority after the user selects one URI.
                    firmwarePackageLauncher.launch(arrayOf("*/*"))
                },
                onOpenAppSettings = {
                    startActivity(
                        Intent(
                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.parse("package:$packageName"),
                        ),
                    )
                },
                onExportModerationReport = { report ->
                    pendingModerationReport = report
                    moderationReportLauncher.launch(report.suggestedFileName())
                },
            )
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(STATE_PERMISSION_CONTINUATION, permissionContinuation.name)
        outState.putString(STATE_BLUETOOTH_CONTINUATION, bluetoothContinuation.name)
        pendingPairingLabel?.let { outState.putString(STATE_PAIRING_LABEL, it) }
        pendingModerationReport?.let {
            outState.putString(STATE_MODERATION_REPORT, ModerationReportCodec.encode(it))
        }
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
            BleContinuation.FINISH_PAIRING -> viewModel.finishPendingPairing()
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
    }
}
