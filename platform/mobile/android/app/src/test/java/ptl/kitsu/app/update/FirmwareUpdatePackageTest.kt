package ptl.kitsu.app.update

import java.io.ByteArrayInputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.zip.CRC32
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

    @Test fun currentLayoutAcceptsTheExactFrozenFirmwareIdentity() {
        val identity = firmwareIdentity()
        assertEquals(331, identity.size)
        assertEquals(
            "KITSU-ID1|schema=1|length=0331|version=0.20.3|" +
                "device_class=heltec-v3.2|layout=kitsu-8m-dual-ota-3m-v1|" +
                "flash=00800000|nvs=00009000/00040000|otadata=00049000/00002000|" +
                "app0=00050000|app1=00350000|slot=00300000|journal=00001000|" +
                "max=002ff000|spiffs=00670000/00140000|conn=007b0000/00040000|" +
                "coredump=007f0000/00010000|crc32=068e9051|end\u0000",
            identity.toString(Charsets.US_ASCII),
        )
        val image = imageWithIdentities(identity)
        withTemporaryImage(image) { imageFile ->
            FirmwareUpdatePackageReader.validateFirmwareIdentity(
                imageFile,
                firmwareManifest(image),
            )
        }
    }

    @Test fun legacyLayoutRemainsReadableWithoutAnIdentityMarker() {
        val image = validEsp32S3Image(ByteArray(1_536))
        withTemporaryImage(image) { imageFile ->
            FirmwareUpdatePackageReader.validateFirmwareIdentity(
                imageFile,
                firmwareManifest(
                    image,
                    partitionBytes = FirmwareUpdatePackageReader.LEGACY_PARTITION_BYTES,
                ),
            )
        }
    }

    @Test fun currentLayoutRejectsMissingAndDuplicateIdentityMarkers() {
        val missing = validEsp32S3Image(ByteArray(1_536))
        assertEquals(
            "invalid_firmware_identity",
            identityFailureCode(missing, firmwareManifest(missing)),
        )

        val duplicate = imageWithIdentities(firmwareIdentity(), firmwareIdentity())
        assertEquals(
            "invalid_firmware_identity",
            identityFailureCode(duplicate, firmwareManifest(duplicate)),
        )
    }

    @Test fun currentLayoutRejectsCorruptUnboundedAndNoncanonicalIdentities() {
        val badCrc = firmwareIdentity().also { record ->
            val lastCrcDigit = record.size - "|end\u0000".toByteArray(Charsets.US_ASCII).size - 1
            record[lastCrcDigit] = if (record[lastCrcDigit] == '0'.code.toByte()) {
                '1'.code.toByte()
            } else {
                '0'.code.toByte()
            }
        }
        val unbounded = "KITSU-ID1|".toByteArray(Charsets.US_ASCII) +
            ByteArray(376) { 'a'.code.toByte() } + byteArrayOf(0)
        val uppercaseHex = firmwareIdentity(mapOf("app0" to "0005000A"))

        listOf(badCrc, unbounded, uppercaseHex).forEach { identity ->
            val image = imageWithIdentities(identity)
            assertEquals(
                "invalid_firmware_identity",
                identityFailureCode(image, firmwareManifest(image)),
            )
        }
    }

    @Test fun currentLayoutBindsIdentityVersionDeviceLayoutAndSchemaToTheManifest() {
        val wrongIdentityFields = listOf(
            mapOf("version" to "0.20.4"),
            mapOf("device_class" to "heltec-v3.1"),
            mapOf("layout" to "kitsu-8m-dual-ota-3m-v2"),
            mapOf("schema" to "2"),
        )
        wrongIdentityFields.forEach { overrides ->
            val image = imageWithIdentities(firmwareIdentity(overrides))
            assertEquals(
                "firmware_identity_mismatch",
                identityFailureCode(image, firmwareManifest(image)),
            )
        }

        val validImage = imageWithIdentities(firmwareIdentity())
        assertEquals(
            "firmware_identity_mismatch",
            identityFailureCode(
                validImage,
                firmwareManifest(validImage).copy(firmwareVersion = "0.20.4"),
            ),
        )
        assertEquals(
            "firmware_identity_mismatch",
            identityFailureCode(
                validImage,
                firmwareManifest(validImage).copy(deviceClass = "heltec-v3.2"),
            ),
        )
    }

    @Test fun currentLayoutBindsEveryAddressAndSizeInTheFirmwareIdentity() {
        val wrongGeometry = listOf(
            mapOf("flash" to "007fffff"),
            mapOf("nvs_offset" to "0000a000"),
            mapOf("nvs_bytes" to "0003f000"),
            mapOf("otadata_offset" to "00048000"),
            mapOf("otadata_bytes" to "00001000"),
            mapOf("app0" to "00051000"),
            mapOf("app1" to "00351000"),
            mapOf("slot" to "002ff000"),
            mapOf("journal" to "00002000"),
            mapOf("max" to "002fe000"),
            mapOf("spiffs_offset" to "00671000"),
            mapOf("spiffs_bytes" to "0013f000"),
            mapOf("conn_offset" to "007b1000"),
            mapOf("conn_bytes" to "0003f000"),
            mapOf("coredump_offset" to "007f1000"),
            mapOf("coredump_bytes" to "0000f000"),
        )
        wrongGeometry.forEach { overrides ->
            val image = imageWithIdentities(firmwareIdentity(overrides))
            assertEquals(
                overrides.toString(),
                "firmware_identity_mismatch",
                identityFailureCode(image, firmwareManifest(image)),
            )
        }
    }

    @Test fun identityFailureDeletesTheStagedCurrentLayoutImage() {
        val image = validEsp32S3Image(ByteArray(1_536))
        val destination = java.io.File.createTempFile("kitsu-identity-cleanup", ".bin")
        destination.writeBytes(image)
        val failure = runCatching {
            FirmwareUpdatePackageReader.validateStagedImage(
                destination,
                image.size,
                firmwareManifest(image),
            )
        }.exceptionOrNull() as FirmwarePackageException
        assertEquals("invalid_firmware_identity", failure.code)
        assertFalse(destination.exists())
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

    private fun validEsp32S3Image(
        segmentData: ByteArray = byteArrayOf(1, 2, 3, 4),
    ): ByteArray {
        require(segmentData.isNotEmpty() && segmentData.size % 4 == 0)
        val header = ByteArray(24)
        header[0] = 0xE9.toByte()
        header[1] = 1
        header[12] = 9
        header[23] = 1
        val segmentHeader = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).apply {
            putInt(0x3f001000)
            putInt(segmentData.size)
        }.array()
        val body = header + segmentHeader + segmentData
        val checksumOffset = body.size + (15 - (body.size % 16))
        val prefix = body.copyOf(checksumOffset + 1)
        prefix[checksumOffset] = segmentData.fold(0xEF) { checksum, byte ->
            checksum xor (byte.toInt() and 0xff)
        }.toByte()
        return prefix + MessageDigest.getInstance("SHA-256").digest(prefix)
    }

    private fun imageWithIdentities(vararg identities: ByteArray): ByteArray {
        val data = ByteArray(1_536)
        identities.forEachIndexed { index, identity ->
            val offset = 64 + (index * 512)
            require(offset + identity.size <= data.size)
            identity.copyInto(data, destinationOffset = offset)
        }
        return validEsp32S3Image(data)
    }

    private fun firmwareIdentity(overrides: Map<String, String> = emptyMap()): ByteArray {
        fun field(name: String, fallback: String): String = overrides[name] ?: fallback
        var prefix =
            "KITSU-ID1|schema=${field("schema", "1")}|length=0000|" +
                "version=${field("version", "0.20.3")}|" +
                "device_class=${field("device_class", "heltec-v3.2")}|" +
                "layout=${field("layout", "kitsu-8m-dual-ota-3m-v1")}|" +
                "flash=${field("flash", "00800000")}|" +
                "nvs=${field("nvs_offset", "00009000")}/${field("nvs_bytes", "00040000")}|" +
                "otadata=${field("otadata_offset", "00049000")}/" +
                "${field("otadata_bytes", "00002000")}|" +
                "app0=${field("app0", "00050000")}|app1=${field("app1", "00350000")}|" +
                "slot=${field("slot", "00300000")}|journal=${field("journal", "00001000")}|" +
                "max=${field("max", "002ff000")}|" +
                "spiffs=${field("spiffs_offset", "00670000")}/" +
                "${field("spiffs_bytes", "00140000")}|" +
                "conn=${field("conn_offset", "007b0000")}/${field("conn_bytes", "00040000")}|" +
                "coredump=${field("coredump_offset", "007f0000")}/" +
                "${field("coredump_bytes", "00010000")}"
        val totalBytes = prefix.toByteArray(Charsets.US_ASCII).size +
            "|crc32=00000000|end\u0000".toByteArray(Charsets.US_ASCII).size
        prefix = prefix.replace("length=0000", "length=${totalBytes.toString().padStart(4, '0')}")
        val prefixBytes = prefix.toByteArray(Charsets.US_ASCII)
        val checksum = CRC32().apply { update(prefixBytes) }.value.toString(16).padStart(8, '0')
        return "$prefix|crc32=$checksum|end\u0000".toByteArray(Charsets.US_ASCII)
    }

    private fun firmwareManifest(
        image: ByteArray,
        firmwareVersion: String = "0.20.3",
        partitionBytes: Int = FirmwareUpdatePackageReader.CURRENT_PARTITION_BYTES,
    ): FirmwareUpdateManifest = FirmwareUpdateManifest(
        schema = FirmwareUpdatePackageReader.MANIFEST_SCHEMA,
        releaseId = "identity-test",
        firmwareVersion = firmwareVersion,
        deviceClass = FirmwareUpdatePackageReader.DEVICE_CLASS,
        imageFormat = FirmwareUpdatePackageReader.IMAGE_FORMAT,
        imageBytes = image.size,
        imageSha256 = sha256(image),
        partitionBytes = partitionBytes,
        chunkBytes = FirmwareUpdatePackageReader.CHUNK_BYTES,
        rollback = true,
    )

    private fun identityFailureCode(
        image: ByteArray,
        manifest: FirmwareUpdateManifest,
    ): String = withTemporaryImage(image) { imageFile ->
        val failure = runCatching {
            FirmwareUpdatePackageReader.validateFirmwareIdentity(imageFile, manifest)
        }.exceptionOrNull() as FirmwarePackageException
        failure.code
    }

    private fun <T> withTemporaryImage(image: ByteArray, block: (java.io.File) -> T): T {
        val imageFile = java.io.File.createTempFile("kitsu-identity", ".bin")
        return try {
            imageFile.writeBytes(image)
            block(imageFile)
        } finally {
            imageFile.delete()
        }
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
