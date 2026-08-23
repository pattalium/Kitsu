package ptl.kitsu.app.update

import java.io.InputStream
import java.security.MessageDigest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

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

    private fun hex(value: String): ByteArray = ByteArray(value.length / 2) { index ->
        value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}
