package ptl.kitsu.app.update

import java.io.ByteArrayInputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.firmwarePackagePickerMimeTypes

class FirmwareUpdatePackageTest {
    @Test fun onlyActiveTransferStagesLockCompanionControls() {
        val locked = setOf(
            FirmwareInstallStage.PREPARING,
            FirmwareInstallStage.TRANSFERRING,
            FirmwareInstallStage.VERIFYING,
            FirmwareInstallStage.READY_TO_REBOOT,
            FirmwareInstallStage.REBOOTING,
        )
        FirmwareInstallStage.entries.forEach { stage ->
            assertEquals(stage in locked, stage.locksCompanionControls)
        }
    }

    @Test fun zeroLengthReadCannotSpinForever() {
        val destination = java.io.File.createTempFile("kitsu-update", ".bin")
        destination.delete()
        val zeroReader = object : InputStream() {
            override fun read(): Int = 0
            override fun read(buffer: ByteArray, offset: Int, length: Int): Int = 0
        }
        val failure = runCatching {
            FirmwareUpdatePackageReader.read(zeroReader, destination)
        }.exceptionOrNull() as FirmwarePackageException
        assertEquals("truncated_header", failure.code)
        assertFalse(destination.exists())
    }

    @Test fun bundledVerifierAcceptsTheRfc8032EmptyMessageVector() {
        val publicKey = hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        val spki = hex("302a300506032b6570032100") + publicKey
        val signature = hex(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        )
        assertTrue(verifyEd25519Spki(spki, byteArrayOf(), signature))
    }

    @Test fun esp32S3ImageValidatorChecksTheRomLayoutAndAppendedDigest() {
        val destination = java.io.File.createTempFile("kitsu-esp32s3", ".bin")
        try {
            destination.writeBytes(validEsp32S3Image())
            FirmwareUpdatePackageReader.validateEsp32S3Image(destination, destination.length().toInt())

            val wrongChip = destination.readBytes().also { it[12] = 8 }
            destination.writeBytes(wrongChip)
            val failure = runCatching {
                FirmwareUpdatePackageReader.validateEsp32S3Image(destination, wrongChip.size)
            }.exceptionOrNull() as FirmwarePackageException
            assertEquals("invalid_esp_image", failure.code)
        } finally {
            destination.delete()
        }
    }

    @Test fun firmwareVersionMatchesTheDeviceThirtyTwoByteLimit() {
        assertTrue(FirmwareUpdatePackageReader.isSupportedFirmwareVersion("1.2.3+${"a".repeat(26)}"))
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareVersion("1.2.3+${"a".repeat(27)}"))
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareVersion("01.2.3"))
    }

    @Test fun boundedSemVerProfileAcceptsStrictPrereleaseBuildAndLargeCoreValues() {
        listOf(
            "1.0.0-alpha.0",
            "1.0.0-x-y-z.--",
            "1.0.0+001",
            "2147483648.999999999.0",
            "9223372036854775807.0.0",
        ).forEach { version ->
            assertTrue(version, FirmwareUpdatePackageReader.isSupportedFirmwareVersion(version))
        }

        assertFalse(
            FirmwareUpdatePackageReader.isSupportedFirmwareVersion("9223372036854775808.0.0"),
        )
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareVersion("0.20.3+build.é"))
    }

    @Test fun bothReleasedOtaSlotSizesAreAcceptedExactly() {
        assertEquals(
            0x32F000,
            FirmwareUpdatePackageReader.maximumImageBytes(
                FirmwareUpdatePackageReader.LEGACY_PARTITION_BYTES,
            ),
        )
        assertEquals(
            0x2FF000,
            FirmwareUpdatePackageReader.maximumImageBytes(
                FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES,
            ),
        )
    }

    @Test fun unsupportedOtaSlotSizesAreRejectedFailClosed() {
        listOf(
            0,
            FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES - 1,
            FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES + 1,
            FirmwareUpdatePackageReader.LEGACY_PARTITION_BYTES - 1,
            FirmwareUpdatePackageReader.LEGACY_PARTITION_BYTES + 1,
            Int.MAX_VALUE,
        ).forEach { partitionBytes ->
            assertEquals(null, FirmwareUpdatePackageReader.maximumImageBytes(partitionBytes))
        }
    }

    @Test fun imageAndPrivateJournalMustFitTheDeclaredSlot() {
        assertTrue(FirmwareUpdatePackageReader.isSupportedFirmwareLayout(0x330000, 0x32F000))
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareLayout(0x330000, 0x32F001))
        assertTrue(FirmwareUpdatePackageReader.isSupportedFirmwareLayout(0x300000, 0x2FF000))
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareLayout(0x300000, 0x2FF001))
        assertFalse(FirmwareUpdatePackageReader.isSupportedFirmwareLayout(0x300000, 0))
    }

    @Test fun runningFirmwareVersionSelectsTheExactPhysicalSlotContract() {
        val legacy = listOf(
            "0.20.2",
            "0.20.2+build.7",
            "0.20.3-0",
            "0.20.3-rc.1",
            "0.20.3-rc.1+build.7",
            "0.20.3-alpha-beta.1+001",
        )
        legacy.forEach { version ->
            assertTrue(version, FirmwareUpdatePackageReader.isSupportedFirmwareVersion(version))
            assertTrue(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x330000))
            assertFalse(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x300000))
        }

        val current = listOf(
            "0.20.3",
            "0.20.3+build.7",
            "0.20.3+001",
            "0.20.4-0",
            "0.20.4-rc.1+build.7",
            "0.21.0-rc.1",
            "1.0.0-0",
        )
        current.forEach { version ->
            assertTrue(version, FirmwareUpdatePackageReader.isSupportedFirmwareVersion(version))
            assertTrue(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x300000))
            assertFalse(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x330000))
        }

        val rejected = listOf(
            null,
            "",
            " 0.20.3",
            "0.20.3 ",
            "v0.20.3",
            "0.20",
            "0.20.3.0",
            "00.20.3",
            "0.020.3",
            "0.20.03",
            "0.20.3-",
            "0.20.3+",
            "0.20.3-.",
            "0.20.3+.",
            "0.20.3-rc.",
            "0.20.3+build.",
            "0.20.3-rc..1",
            "0.20.3+build..7",
            "0.20.3-01",
            "0.20.3-rc.01",
            "0.20.3-rc_1",
            "0.20.3+build/7",
            "0.20.3-rc+build+more",
            "999999999999999999999.0.0",
            "1.2.3+${"a".repeat(27)}",
        )
        rejected.forEach { version ->
            if (version != null) {
                assertFalse(version, FirmwareUpdatePackageReader.isSupportedFirmwareVersion(version))
            }
            assertFalse(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x330000))
            assertFalse(FirmwareUpdatePackageReader.isLayoutCompatibleWithDevice(version, 0x300000))
        }
    }

    @Test fun parserAcceptsBothSlotContractsBeforeEnforcingThePinnedSigner() {
        listOf(0x330000, 0x300000).forEach { partitionBytes ->
            assertEquals(
                "invalid_manifest_signature",
                parseFailureCode(unsignedPackage(partitionBytes, imageBytes = 80)),
            )
        }
    }

    @Test fun parserRejectsWrongSlotSizesBeforeSignatureOrImageProcessing() {
        listOf(0, 0x2FFFFF, 0x300001, 0x32FFFF, 0x330001, Int.MAX_VALUE).forEach { partitionBytes ->
            assertEquals(
                "unsupported_manifest",
                parseFailureCode(unsignedPackage(partitionBytes, imageBytes = 80)),
            )
        }
    }

    @Test fun parserBindsImageAndJournalToTheManifestDeclaredSlot() {
        assertEquals(
            "unsupported_manifest",
            parseFailureCode(unsignedPackage(0x300000, imageBytes = 0x2FF001)),
        )
        assertEquals(
            "invalid_image_length",
            parseFailureCode(unsignedPackage(0x330000, imageBytes = 0x32F001)),
        )
        assertEquals(
            "invalid_image_length",
            parseFailureCode(unsignedPackage(0x300000, imageBytes = 0)),
        )
    }

    @Test fun kitsuFwPickerDoesNotDependOnAProviderSpecificMimeType() {
        assertArrayEquals(arrayOf("*/*"), firmwarePackagePickerMimeTypes())
    }

    private fun validEsp32S3Image(): ByteArray {
        val prefix = ByteArray(48)
        prefix[0] = 0xE9.toByte()
        prefix[1] = 1
        prefix[12] = 9
        prefix[23] = 1
        // One four-byte segment starts after its little-endian header at byte 32.
        prefix[24] = 0x00
        prefix[25] = 0x10
        prefix[26] = 0x00
        prefix[27] = 0x3f
        prefix[28] = 4
        prefix[32] = 1
        prefix[33] = 2
        prefix[34] = 3
        prefix[35] = 4
        prefix[47] = (0xEF xor 1 xor 2 xor 3 xor 4).toByte()
        return prefix + MessageDigest.getInstance("SHA-256").digest(prefix)
    }

    private fun unsignedPackage(partitionBytes: Int, imageBytes: Int): ByteArray {
        val manifest = (
            "{\"schema\":\"kitsu.ble-firmware.v1\",\"release_id\":\"layout-test\"," +
                "\"firmware_version\":\"0.20.3\",\"device_class\":" +
                "\"heltec-wifi-lora-32-v3-esp32s3-8mb\",\"image_format\":\"esp32s3-app\"," +
                "\"image_bytes\":$imageBytes,\"image_sha256\":\"${"0".repeat(64)}\"," +
                "\"partition_bytes\":$partitionBytes,\"chunk_bytes\":4096,\"rollback\":true}"
            ).toByteArray(Charsets.UTF_8)
        val header = ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN).apply {
            put("KITSUFW1".toByteArray(Charsets.US_ASCII))
            putInt(manifest.size)
            putShort(64.toShort())
            putShort(0.toShort())
            putInt(imageBytes)
        }.array()
        // A structurally valid Ed25519 signature for a different message. It
        // reaches the production authority check without adding a test signer.
        val wrongSignature = hex(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        )
        return header + manifest + wrongSignature
    }

    private fun parseFailureCode(packageBytes: ByteArray): String {
        val destination = java.io.File.createTempFile("kitsu-package-policy", ".bin")
        destination.delete()
        return try {
            val failure = runCatching {
                FirmwareUpdatePackageReader.read(ByteArrayInputStream(packageBytes), destination)
            }.exceptionOrNull() as FirmwarePackageException
            failure.code
        } finally {
            destination.delete()
        }
    }

    private fun hex(value: String): ByteArray = ByteArray(value.length / 2) { index ->
        value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}
