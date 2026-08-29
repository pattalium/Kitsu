package ptl.kitsu.app.repository

import java.io.File
import java.util.ArrayDeque
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
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
import ptl.kitsu.app.update.FirmwareInstallProgress
import ptl.kitsu.app.update.FirmwareInstallStage
import ptl.kitsu.app.update.FirmwareUpdateManifest
import ptl.kitsu.app.update.FirmwareUpdatePackageReader
import ptl.kitsu.app.update.FirmwareUpdateReceipt
import ptl.kitsu.app.update.VerifiedFirmwarePackage

class OwnerRepositoryFirmwareReinstallTest {
    @Test fun confirmedSamePackageWithoutExplicitReinstallIsReadOnly() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("confirmed")
            val progress = mutableListOf<FirmwareInstallProgress>()

            fixture.repository.installFirmware(fixture.packageFile, onProgress = progress::add)

            assertEquals(listOf("firmware.update.status"), fixture.transport.firmwareCalls)
            assertEquals(FirmwareInstallStage.COMPLETE, progress.last().stage)
        } finally {
            fixture.close()
        }
    }

    @Test fun explicitConfirmedReinstallWritesOnceAndConfirmsTheOtherSlot() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("confirmed")
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")
            val progress = mutableListOf<FirmwareInstallProgress>()

            fixture.repository.installFirmware(
                fixture.packageFile,
                reinstallConfirmed = true,
                onProgress = progress::add,
            )

            assertEquals(
                listOf(
                    "firmware.update.status",
                    "firmware.update.begin",
                    "firmware.update.write:0:80",
                    "firmware.update.finish",
                    "firmware.update.reboot",
                    "firmware.update.status",
                    "firmware.update.status",
                ),
                fixture.transport.firmwareCalls,
            )
            assertEquals(1, fixture.transport.firmwareCalls.count { it == "firmware.update.begin" })
            assertEquals(FirmwareInstallStage.COMPLETE, progress.last().stage)
        } finally {
            fixture.close()
        }
    }

    @Test fun ordinaryPendingVerifyContinuationWaitsAndCompletesWithoutBegin() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(
                listOf("firmware.update.status", "firmware.update.status"),
                fixture.transport.firmwareCalls,
            )
        } finally {
            fixture.close()
        }
    }

    @Test fun explicitReinstallRejectsIdleBeforeBegin() = runTest {
        assertExplicitReinstallRejected {
            receipt("idle", updateId = null, imageBytes = 0, nextOffset = 0)
        }
    }

    @Test fun explicitReinstallRejectsPendingVerifyBeforeWaitingOrBeginning() = runTest {
        assertExplicitReinstallRejected { receipt("pending_verify") }
    }

    @Test fun explicitReinstallRejectsReceivingBeforeResumeBegin() = runTest {
        assertExplicitReinstallRejected { receipt("receiving", nextOffset = 40) }
    }

    @Test fun explicitReinstallRejectsReadyBeforeReboot() = runTest {
        assertExplicitReinstallRejected { receipt("ready_to_reboot") }
    }

    @Test fun explicitReinstallRejectsDifferentConfirmedPackageBeforeBegin() = runTest {
        assertExplicitReinstallRejected {
            receipt("confirmed", updateId = "f".repeat(64))
        }
    }

    @Test fun confirmedDifferentPackageStartsANormalInstall() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt(
                state = "confirmed",
                updateId = "f".repeat(64),
            )
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(1, fixture.transport.firmwareCalls.count { it == "firmware.update.begin" })
            assertTrue(fixture.transport.firmwareCalls.contains("firmware.update.write:0:80"))
        } finally {
            fixture.close()
        }
    }

    @Test fun receivingSamePackageResumesAtTheAuthoritativeOffset() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("receiving", nextOffset = 40)
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(1, fixture.transport.firmwareCalls.count { it == "firmware.update.begin" })
            assertTrue(fixture.transport.firmwareCalls.contains("firmware.update.write:40:40"))
            assertTrue(fixture.transport.firmwareCalls.none { it == "firmware.update.write:0:80" })
        } finally {
            fixture.close()
        }
    }

    @Test fun readySamePackageRebootsWithoutBeginningOrRewriting() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("ready_to_reboot")
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(
                listOf(
                    "firmware.update.status",
                    "firmware.update.reboot",
                    "firmware.update.status",
                    "firmware.update.status",
                ),
                fixture.transport.firmwareCalls,
            )
        } finally {
            fixture.close()
        }
    }

    @Test fun rolledBackSamePackageIsResetBeforeOneFreshBegin() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt("rolled_back")
            fixture.transport.statuses += fixture.receipt("idle", updateId = null, imageBytes = 0, nextOffset = 0)
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(
                listOf(
                    "firmware.update.status",
                    "firmware.update.abort",
                    "firmware.update.status",
                    "firmware.update.begin",
                    "firmware.update.write:0:80",
                    "firmware.update.finish",
                    "firmware.update.reboot",
                    "firmware.update.status",
                    "firmware.update.status",
                ),
                fixture.transport.firmwareCalls,
            )
        } finally {
            fixture.close()
        }
    }

    @Test fun unboundFailedStateIsResetBeforeOneFreshBegin() = runTest {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.receipt(
                "failed",
                updateId = null,
                imageBytes = 0,
                nextOffset = 0,
            )
            fixture.transport.statuses += fixture.receipt("idle", updateId = null, imageBytes = 0, nextOffset = 0)
            fixture.transport.statuses += fixture.receipt("pending_verify")
            fixture.transport.statuses += fixture.receipt("confirmed")

            fixture.repository.installFirmware(fixture.packageFile, onProgress = {})

            assertEquals(1, fixture.transport.firmwareCalls.count { it == "firmware.update.abort" })
            assertEquals(1, fixture.transport.firmwareCalls.count { it == "firmware.update.begin" })
            val abortIndex = fixture.transport.firmwareCalls.indexOf("firmware.update.abort")
            val beginIndex = fixture.transport.firmwareCalls.indexOf("firmware.update.begin")
            assertTrue(abortIndex < beginIndex)
        } finally {
            fixture.close()
        }
    }

    @Test fun explicitReinstallRejectsConfirmedImageLengthMismatchBeforeBegin() = runTest {
        assertExplicitReinstallRejected {
            receipt("confirmed", imageBytes = 79, nextOffset = 79)
        }
    }

    @Test fun explicitReinstallRejectsConfirmedIncompleteOffsetBeforeBegin() = runTest {
        assertExplicitReinstallRejected { receipt("confirmed", nextOffset = 79) }
    }

    @Test fun explicitReinstallRejectsConfirmedVersionMismatchBeforeBegin() = runTest {
        assertExplicitReinstallRejected {
            receipt("confirmed", firmwareVersion = "0.20.4")
        }
    }

    private suspend fun TestScope.assertExplicitReinstallRejected(
        initialStatus: Fixture.() -> FirmwareUpdateReceipt,
    ) {
        val fixture = fixture()
        try {
            fixture.transport.statuses += fixture.initialStatus()
            val failure = try {
                fixture.repository.installFirmware(
                    fixture.packageFile,
                    reinstallConfirmed = true,
                    onProgress = {},
                )
                fail("expected firmware_reinstall_confirmation_stale")
                null
            } catch (error: TransportException) {
                error
            }
            assertEquals("firmware_reinstall_confirmation_stale", failure?.code)
            assertEquals(listOf("firmware.update.status"), fixture.transport.firmwareCalls)
        } finally {
            fixture.close()
        }
    }

    private suspend fun TestScope.fixture(): Fixture {
        val imageFile = File.createTempFile("kitsu-reinstall", ".bin").apply {
            writeBytes(ByteArray(IMAGE_BYTES) { it.toByte() })
        }
        val packageFile = firmwarePackage(imageFile)
        val transport = ScriptedFirmwareTransport(packageFile.updateId)
        val coordinator = ConnectionCoordinator(transport)
        assertTrue(coordinator.connect(userInitiated = true).connected)
        transport.firmwareCalls.clear()
        val repository = OwnerRepository(
            coordinator = coordinator,
            cache = EmptyCache,
            credentials = EmptyCredentials,
            pairingService = UnusedPairingService,
            scope = backgroundScope,
        )
        return Fixture(packageFile, transport, repository, imageFile)
    }

    private fun firmwarePackage(imageFile: File): VerifiedFirmwarePackage {
        val manifestBytes = "{\"release_id\":\"reinstall-test\"}".toByteArray(Charsets.UTF_8)
        return VerifiedFirmwarePackage(
            manifest = FirmwareUpdateManifest(
                schema = FirmwareUpdatePackageReader.MANIFEST_SCHEMA,
                releaseId = "reinstall-test",
                firmwareVersion = FIRMWARE_VERSION,
                deviceClass = FirmwareUpdatePackageReader.DEVICE_CLASS,
                imageFormat = FirmwareUpdatePackageReader.IMAGE_FORMAT,
                imageBytes = IMAGE_BYTES,
                imageSha256 = "0".repeat(64),
                partitionBytes = FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES,
                chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
                rollback = true,
            ),
            manifestBytes = manifestBytes,
            signature = ByteArray(64),
            imageFile = imageFile,
        )
    }

    private data class Fixture(
        val packageFile: VerifiedFirmwarePackage,
        val transport: ScriptedFirmwareTransport,
        val repository: OwnerRepository,
        private val imageFile: File,
    ) {
        fun receipt(
            state: String,
            updateId: String? = if (state == "idle") null else packageFile.updateId,
            imageBytes: Int = if (state == "idle") 0 else IMAGE_BYTES,
            nextOffset: Int = when (state) {
                "idle" -> 0
                "receiving" -> 0
                else -> imageBytes
            },
            firmwareVersion: String = FIRMWARE_VERSION,
        ): FirmwareUpdateReceipt = FirmwareUpdateReceipt(
            ok = true,
            protocol = 1,
            state = state,
            updateId = updateId,
            firmwareVersion = firmwareVersion,
            imageBytes = imageBytes,
            nextOffset = nextOffset,
            chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
            resumed = state == "receiving" && nextOffset > 0,
            replayed = false,
            scheduled = false,
        )

        fun close() {
            imageFile.delete()
        }
    }

    private class ScriptedFirmwareTransport(
        private val packageUpdateId: String,
        private val delegate: MockKitsuTransport = MockKitsuTransport(),
    ) : KitsuTransport by delegate {
        val statuses = ArrayDeque<FirmwareUpdateReceipt>()
        val firmwareCalls = mutableListOf<String>()
        private var current = receipt("idle", null, 0, 0)

        override suspend fun connect(): ConnectResult = delegate.connect()

        override suspend fun firmwareUpdateStatus(): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.status"
            check(statuses.isNotEmpty()) { "missing scripted firmware status" }
            return statuses.removeFirst().also { current = it }
        }

        override suspend fun beginFirmwareUpdate(
            manifest: ByteArray,
            signature: ByteArray,
        ): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.begin"
            val resumedOffset = current.nextOffset.takeIf {
                current.state == "receiving" && current.updateId == packageUpdateId
            } ?: 0
            return receipt("receiving", packageUpdateId, IMAGE_BYTES, resumedOffset).also {
                current = it
            }
        }

        override suspend fun writeFirmwareUpdate(
            updateId: String,
            offset: Int,
            data: ByteArray,
        ): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.write:$offset:${data.size}"
            check(updateId == packageUpdateId)
            check(offset == current.nextOffset)
            return receipt("receiving", packageUpdateId, IMAGE_BYTES, offset + data.size).also {
                current = it
            }
        }

        override suspend fun finishFirmwareUpdate(updateId: String): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.finish"
            check(updateId == packageUpdateId)
            return receipt("ready_to_reboot", packageUpdateId, IMAGE_BYTES, IMAGE_BYTES).also {
                current = it
            }
        }

        override suspend fun rebootFirmwareUpdate(updateId: String): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.reboot"
            check(updateId == packageUpdateId)
            return receipt("ready_to_reboot", packageUpdateId, IMAGE_BYTES, IMAGE_BYTES)
                .copy(scheduled = true)
                .also { current = it }
        }

        override suspend fun abortFirmwareUpdate(updateId: String): FirmwareUpdateReceipt {
            firmwareCalls += "firmware.update.abort"
            return receipt("idle", null, 0, 0).also { current = it }
        }

        private fun receipt(
            state: String,
            updateId: String?,
            imageBytes: Int,
            nextOffset: Int,
        ) = FirmwareUpdateReceipt(
            ok = true,
            protocol = 1,
            state = state,
            updateId = updateId,
            firmwareVersion = FIRMWARE_VERSION,
            imageBytes = imageBytes,
            nextOffset = nextOffset,
            chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
            resumed = state == "receiving" && nextOffset > 0,
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

    private companion object {
        const val IMAGE_BYTES = 80
        const val FIRMWARE_VERSION = "0.20.3"
    }
}
