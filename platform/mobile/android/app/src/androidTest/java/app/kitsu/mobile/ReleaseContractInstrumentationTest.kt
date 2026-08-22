package app.kitsu.mobile

import android.Manifest
import android.graphics.drawable.AdaptiveIconDrawable
import android.os.Build
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import app.kitsu.mobile.connection.AndroidReconnectSuppressionStore
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.model.ActionCommand
import app.kitsu.mobile.model.ActionKind
import app.kitsu.mobile.model.ActionReceipt
import app.kitsu.mobile.model.ControllerForgetReceipt
import app.kitsu.mobile.model.EventEnvelope
import app.kitsu.mobile.model.HistoryPage
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.MessagePage
import app.kitsu.mobile.model.MeshChannel
import app.kitsu.mobile.model.MeshConfigurationReceipt
import app.kitsu.mobile.model.PeerPage
import app.kitsu.mobile.transport.ConnectResult
import app.kitsu.mobile.transport.ConnectionMode
import app.kitsu.mobile.transport.KitsuTransport
import app.kitsu.mobile.update.FirmwareUpdateReceipt
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.emptyFlow
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ReleaseContractInstrumentationTest {
    @Test fun launcherLoadsAsAnAdaptiveIconOnEverySupportedApi() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val icon = context.packageManager.getApplicationIcon(context.applicationInfo)
        assertTrue(icon is AdaptiveIconDrawable)
        icon as AdaptiveIconDrawable
        assertNotNull(icon.background)
        assertNotNull(icon.foreground)
        if (Build.VERSION.SDK_INT >= 33) assertNotNull(icon.monochrome)
    }

    @Test fun productionIdentityAndProvenanceAreExact() {
        assertEquals(13, BuildConfig.VERSION_CODE)
        assertTrue(BuildConfig.VERSION_NAME.startsWith("2.0.0"))
        assertTrue(
            BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256 == "unbound" ||
                Regex("^[0-9a-f]{64}$").matches(BuildConfig.KITSU_SOURCE_ARCHIVE_SHA256),
        )
    }

    @Test fun packagedAppHasNoNetworkOrForegroundServicePermission() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val packageInfo = context.packageManager.getPackageInfo(
            context.packageName,
            android.content.pm.PackageManager.GET_PERMISSIONS or android.content.pm.PackageManager.GET_SERVICES,
        )
        val permissions = packageInfo.requestedPermissions?.toSet().orEmpty()
        assertFalse(Manifest.permission.INTERNET in permissions)
        assertFalse(Manifest.permission.FOREGROUND_SERVICE in permissions)
        assertTrue(packageInfo.services.isNullOrEmpty())
    }

    @Test fun disconnectSuppressionSurvivesColdServiceRecreation() = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val preferences = context.getSharedPreferences("kitsu_disconnect_${System.nanoTime()}", 0)
        try {
            ConnectionCoordinator(ProbeTransport(), AndroidReconnectSuppressionStore(preferences))
                .disconnect(suppressAutomaticReconnect = true)
            val direct = ProbeTransport()
            val cold = ConnectionCoordinator(direct, AndroidReconnectSuppressionStore(preferences))
            assertEquals("user_disconnected", cold.connect(userInitiated = false).detail)
            assertEquals(0, direct.connectCount)
            assertTrue(cold.connect(userInitiated = true).connected)
        } finally {
            preferences.edit().clear().commit()
        }
    }

    private class ProbeTransport : KitsuTransport {
        override val mode = ConnectionMode.DIRECT_BLE
        var connectCount = 0
        override suspend fun connect() = ConnectResult.Connected.also { connectCount++ }
        override suspend fun disconnect() = Unit
        override suspend fun status() = KitsuStatus(deviceId = "KTDEAD", companionName = "Kitsu", updatedAt = 1)
        override suspend fun history(after: String?, limit: Int) = HistoryPage()
        override suspend fun peers() = PeerPage()
        override suspend fun messages(after: String?, limit: Int) = MessagePage()
        override suspend fun action(command: ActionCommand) = ActionReceipt(
            command.clientRequestId,
            true,
            if (command.kind == ActionKind.SEND_MESSAGE) "queued" else "applied",
        )
        override fun events(after: String?): Flow<EventEnvelope> = emptyFlow()
        override suspend fun channels(): List<MeshChannel> = emptyList()
        override suspend fun configureMesh(enabled: Boolean) = MeshConfigurationReceipt(enabled, "uk_eu_narrow", 22)
        override suspend fun forgetController() = ControllerForgetReceipt("kitsu.controller-forget.v1", true)
        override suspend fun firmwareUpdateStatus() = updateReceipt()
        override suspend fun beginFirmwareUpdate(manifest: ByteArray, signature: ByteArray) = updateReceipt()
        override suspend fun writeFirmwareUpdate(updateId: String, offset: Int, data: ByteArray) = updateReceipt()
        override suspend fun finishFirmwareUpdate(updateId: String) = updateReceipt()
        override suspend fun rebootFirmwareUpdate(updateId: String) = updateReceipt().copy(scheduled = true)
        override suspend fun abortFirmwareUpdate(updateId: String) = updateReceipt()
        private fun updateReceipt() = FirmwareUpdateReceipt(
            true, 1, "idle", null, "0.0.0", 0, 0, 4_096, false, false, false,
        )
    }
}
