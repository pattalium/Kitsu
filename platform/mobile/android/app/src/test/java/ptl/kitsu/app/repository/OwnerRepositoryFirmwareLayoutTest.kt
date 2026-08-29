package ptl.kitsu.app.repository

import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.MockKitsuTransport
import ptl.kitsu.app.transport.TransportException
import ptl.kitsu.app.update.FirmwareUpdateManifest
import ptl.kitsu.app.update.FirmwareUpdatePackageReader
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import ptl.kitsu.app.update.VerifiedFirmwarePackage

class OwnerRepositoryFirmwareLayoutTest {
    @Test fun legacyDeviceRejectsCurrentLayoutBeforeAnyMutatingUpdateOperation() = runBlocking {
        assertLayoutRejectedBeforeMutation("0.20.2", FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES)
    }

    @Test fun migratedDeviceRejectsLegacyLayoutBeforeAnyMutatingUpdateOperation() = runBlocking {
        assertLayoutRejectedBeforeMutation("0.20.3", FirmwareUpdatePackageReader.LEGACY_PARTITION_BYTES)
    }

    @Test fun malformedDeviceVersionRejectsBeforeAnyMutatingUpdateOperation() = runBlocking {
        assertLayoutRejectedBeforeMutation("0.20", FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES)
    }

    @Test fun overlengthDeviceVersionRejectsBeforeAnyMutatingUpdateOperation() = runBlocking {
        assertLayoutRejectedBeforeMutation(
            "1.2.3+${"a".repeat(27)}",
            FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES,
        )
    }

    private suspend fun assertLayoutRejectedBeforeMutation(
        runningFirmwareVersion: String,
        packagePartitionBytes: Int,
    ) {
        val transport = RecordingFirmwareTransport(runningFirmwareVersion)
        val coordinator = ConnectionCoordinator(transport)
        assertEquals(true, coordinator.connect(userInitiated = true).connected)
        transport.calls.clear()
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val imageFile = File.createTempFile("kitsu-layout-order", ".bin")
        val repository = OwnerRepository(
            coordinator = coordinator,
            cache = EmptyCache,
            credentials = EmptyCredentials,
            pairingService = UnusedPairingService,
            scope = scope,
        )
        try {
            val failure = try {
                repository.installFirmware(
                    packageFile = firmwarePackage(packagePartitionBytes, imageFile),
                    onProgress = {},
                )
                fail("expected firmware_layout_mismatch")
                null
            } catch (error: TransportException) {
                error
            }
            assertEquals("firmware_layout_mismatch", failure?.code)
            assertEquals(listOf("firmware.update.status"), transport.calls)
        } finally {
            scope.cancel()
            imageFile.delete()
        }
    }

    private fun firmwarePackage(partitionBytes: Int, imageFile: File): VerifiedFirmwarePackage {
        val manifestBytes = "{}".toByteArray(Charsets.UTF_8)
        return VerifiedFirmwarePackage(
            manifest = FirmwareUpdateManifest(
                schema = FirmwareUpdatePackageReader.MANIFEST_SCHEMA,
                releaseId = "layout-order-test",
                firmwareVersion = "0.20.3",
                deviceClass = FirmwareUpdatePackageReader.DEVICE_CLASS,
                imageFormat = FirmwareUpdatePackageReader.IMAGE_FORMAT,
                imageBytes = 80,
                imageSha256 = "0".repeat(64),
                partitionBytes = partitionBytes,
                chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
                rollback = true,
            ),
            manifestBytes = manifestBytes,
            signature = ByteArray(64),
            imageFile = imageFile,
        )
    }

    private class RecordingFirmwareTransport(
        private val runningFirmwareVersion: String,
        private val delegate: MockKitsuTransport = MockKitsuTransport(),
    ) : KitsuTransport by delegate {
        val calls = mutableListOf<String>()

        override suspend fun connect(): ConnectResult = delegate.connect()

        override suspend fun firmwareUpdateStatus(): FirmwareUpdateReceipt =
            receipt("idle", null).also { calls += "firmware.update.status" }

        override suspend fun beginFirmwareUpdate(
            manifest: ByteArray,
            signature: ByteArray,
        ): FirmwareUpdateReceipt = receipt("receiving", "update").also {
            calls += "firmware.update.begin"
        }

        override suspend fun writeFirmwareUpdate(
            updateId: String,
            offset: Int,
            data: ByteArray,
        ): FirmwareUpdateReceipt = receipt("receiving", updateId).also {
            calls += "firmware.update.write"
        }

        override suspend fun finishFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
            receipt("ready_to_reboot", updateId).also { calls += "firmware.update.finish" }

        override suspend fun rebootFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
            receipt("ready_to_reboot", updateId).copy(scheduled = true).also {
                calls += "firmware.update.reboot"
            }

        override suspend fun abortFirmwareUpdate(updateId: String): FirmwareUpdateReceipt =
            receipt("idle", null).also { calls += "firmware.update.abort" }

        private fun receipt(state: String, updateId: String?) = FirmwareUpdateReceipt(
            ok = true,
            protocol = 1,
            state = state,
            updateId = updateId,
            firmwareVersion = runningFirmwareVersion,
            imageBytes = 0,
            nextOffset = 0,
            chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
            resumed = false,
            replayed = false,
            scheduled = false,
        )
    }

    private object EmptyCache : OwnerCache {
        override fun write(snapshot: CacheSnapshot) = Unit
        override fun read(): CacheSnapshot? = null
        override fun clear() = Unit
    }

    private object EmptyCredentials : CredentialStore {
        override suspend fun bondedCompanion(): BondedCompanion? = null
        override suspend fun bondedCompanions(): List<BondedCompanion> = emptyList()
        override suspend fun saveBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? = null
        override suspend fun removeBondedCompanion(deviceAddress: String): Boolean = false
        override suspend fun pendingBondedCompanion(): BondedCompanion? = null
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) = Unit
        override suspend fun pendingControllerForgetAddress(): String? = null
        override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) = Unit
    }

    private object UnusedPairingService : ControllerPairingService {
        override suspend fun pairController(
            label: String,
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun finishPendingPairing(
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun repairBluetoothPairing(
            deviceAddress: String,
            onProgress: (BluetoothPairingRepairProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override fun cancelPairing() = Unit
    }
}
