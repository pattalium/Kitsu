package app.kitsu.mobile

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.kitsu.mobile.ui.KitsuOwnerApp
import app.kitsu.mobile.update.locksCompanionControls
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue

class MainActivity : ComponentActivity() {
    private val viewModel: MainViewModel by viewModels()

    private enum class BleContinuation { CONNECT, PAIR_CONTROLLER, FINISH_PAIRING }

    private var permissionContinuation = BleContinuation.CONNECT
    private var bluetoothContinuation = BleContinuation.CONNECT
    private var pendingPairingLabel: String? = null

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
        ) continueAfterPermission(permissionContinuation)
        else viewModel.reconnectBluetooth()
    }

    private val firmwarePackageLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri -> uri?.let(viewModel::importFirmware) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val firmware by viewModel.firmware.collectAsStateWithLifecycle()
            val updateActive = firmware.progress.stage.locksCompanionControls
            LaunchedEffect(updateActive) {
                if (updateActive) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            KitsuOwnerApp(
                viewModel = viewModel,
                onRequestBlePermissions = { requestBlePermissions(BleContinuation.CONNECT) },
                onEnableBluetooth = { requestBlePermissions(BleContinuation.CONNECT) },
                onOpenLocationSettings = {
                    locationSettingsLauncher.launch(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
                },
                onPairController = { label ->
                    pendingPairingLabel = label
                    requestBlePermissions(BleContinuation.PAIR_CONTROLLER)
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
            )
        }
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
}
