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
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import app.kitsu.mobile.ui.KitsuOwnerApp
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val viewModel: MainViewModel by viewModels()

    private enum class BlePermissionContinuation {
        CONNECT,
        ENABLE_BLUETOOTH,
        PAIR_CONTROLLER,
    }

    private var blePermissionContinuation = BlePermissionContinuation.CONNECT
    private var bluetoothEnableContinuation = BlePermissionContinuation.CONNECT
    private var pendingPairingLabel: String? = null

    private val bluetoothEnableLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) {
        val continuation = bluetoothEnableContinuation
        bluetoothEnableContinuation = BlePermissionContinuation.CONNECT
        when (continuation) {
            BlePermissionContinuation.PAIR_CONTROLLER -> {
                pendingPairingLabel?.let(viewModel::pairController)
                pendingPairingLabel = null
            }
            BlePermissionContinuation.CONNECT,
            BlePermissionContinuation.ENABLE_BLUETOOTH -> viewModel.reconnectBluetooth()
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        val continuation = blePermissionContinuation
        blePermissionContinuation = BlePermissionContinuation.CONNECT
        if (requiredBlePermissions().all {
                ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
            }
        ) {
            continueAfterBlePermission(continuation)
        } else {
            if (continuation == BlePermissionContinuation.PAIR_CONTROLLER) {
                pendingPairingLabel?.let(viewModel::pairController)
                pendingPairingLabel = null
            } else {
                // Let the transport publish PERMISSION_REQUIRED again so the UI remains truthful.
                viewModel.reconnectBluetooth()
            }
        }
    }

    private val authLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        lifecycleScope.launch {
            val data = result.data ?: Intent()
            val accepted = applicationServices.oidc.completeAuthorization(data)
            if (accepted) {
                viewModel.markOwnerSignedIn()
                viewModel.reconnectIfAllowed()
            } else {
                viewModel.refreshOwnerAccountStatus()
            }
        }
    }

    private val applicationServices: AppServices
        get() = (application as KitsuApplication).services

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            KitsuOwnerApp(
                viewModel = viewModel,
                onRequestBlePermissions = {
                    requestBlePermissions(BlePermissionContinuation.CONNECT)
                },
                onEnableBluetooth = {
                    requestBlePermissions(BlePermissionContinuation.ENABLE_BLUETOOTH)
                },
                onPairController = { label ->
                    pendingPairingLabel = label
                    requestBlePermissions(BlePermissionContinuation.PAIR_CONTROLLER)
                },
                onOpenAppSettings = {
                    startActivity(
                        Intent(
                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.parse("package:$packageName"),
                        ),
                    )
                },
                onSignIn = {
                    lifecycleScope.launch {
                        runCatching { applicationServices.oidc.authorizationIntent() }
                            .onSuccess(authLauncher::launch)
                    }
                },
                onSignOut = {
                    lifecycleScope.launch {
                        applicationServices.ownerRepository.handleSignedOut()
                        viewModel.markOwnerSignedOut()
                        applicationServices.oidc.signOut()
                        if (!applicationServices.mobileRelayController.state.value.enabled) {
                            viewModel.reconnectIfAllowed()
                        }
                    }
                },
            )
        }
    }

    private fun requestBlePermissions(continuation: BlePermissionContinuation) {
        val permissions = requiredBlePermissions()
        if (permissions.all {
                ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
            }
        ) {
            continueAfterBlePermission(continuation)
            return
        }
        blePermissionContinuation = continuation
        permissionLauncher.launch(permissions)
    }

    private fun requiredBlePermissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31) {
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private fun continueAfterBlePermission(continuation: BlePermissionContinuation) {
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || adapter.isEnabled) {
            continueWithBluetoothReady(continuation)
            return
        }
        bluetoothEnableContinuation = continuation
        launchBluetoothEnablePrompt()
    }

    private fun continueWithBluetoothReady(continuation: BlePermissionContinuation) {
        when (continuation) {
            BlePermissionContinuation.PAIR_CONTROLLER -> {
                pendingPairingLabel?.let(viewModel::pairController)
                pendingPairingLabel = null
            }
            BlePermissionContinuation.CONNECT,
            BlePermissionContinuation.ENABLE_BLUETOOTH -> viewModel.reconnectBluetooth()
        }
    }

    private fun launchBluetoothEnablePrompt() {
        bluetoothEnableLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
    }
}
