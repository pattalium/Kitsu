package ptl.kitsu.app.update

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.EOFException
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.Base64
import java.util.zip.CRC32
import net.i2p.crypto.eddsa.EdDSAEngine
import net.i2p.crypto.eddsa.EdDSAPublicKey
import net.i2p.crypto.eddsa.spec.EdDSANamedCurveTable
import net.i2p.crypto.eddsa.spec.EdDSAPublicKeySpec

@Serializable
data class FirmwareUpdateManifest(
    val schema: String,
    @SerialName("release_id") val releaseId: String,
    @SerialName("firmware_version") val firmwareVersion: String,
    @SerialName("device_class") val deviceClass: String,
    @SerialName("image_format") val imageFormat: String,
    @SerialName("image_bytes") val imageBytes: Int,
    @SerialName("image_sha256") val imageSha256: String,
    @SerialName("partition_bytes") val partitionBytes: Int,
    @SerialName("chunk_bytes") val chunkBytes: Int,
    val rollback: Boolean,
)

data class VerifiedFirmwarePackage(
    val manifest: FirmwareUpdateManifest,
    val manifestBytes: ByteArray,
    val signature: ByteArray,
    val imageFile: File,
) {
    val updateId: String = sha256(manifestBytes)
}

class FirmwarePackageException(val code: String, cause: Throwable? = null) : Exception(code, cause)

/** Strict, bounded reader for the offline `.kitsu-fw` container. */
object FirmwareUpdatePackageReader {
    private val json = Json { ignoreUnknownKeys = false; explicitNulls = false }
    private val magic = "KITSUFW1".toByteArray(Charsets.US_ASCII)
    private val releaseId = Regex("^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$")
    private val digest = Regex("^[0-9a-f]{64}$")

    private data class BoundedSemVer(
        val major: Long,
        val minor: Long,
        val patch: Long,
        val hasPrerelease: Boolean,
    )

    fun read(input: InputStream, imageFile: File): VerifiedFirmwarePackage {
        try {
            val header = input.readExact(HEADER_BYTES, "truncated_header")
            if (!header.copyOfRange(0, 8).contentEquals(magic)) fail("invalid_package_magic")
            val manifestLength = header.u32(8)
            val signatureLength = header.u16(12)
            val flags = header.u16(14)
            val imageLength = header.u32(16)
            if (manifestLength !in 1..MAX_MANIFEST_BYTES) fail("invalid_manifest_length")
            if (signatureLength != SIGNATURE_BYTES || flags != 0) fail("invalid_package_header")
            if (imageLength !in 1..MAX_SUPPORTED_IMAGE_BYTES) fail("invalid_image_length")

            val manifestBytes = input.readExact(manifestLength, "truncated_manifest")
            val signature = input.readExact(signatureLength, "truncated_signature")
            val manifest = runCatching {
                json.decodeFromString(FirmwareUpdateManifest.serializer(), manifestBytes.toString(Charsets.UTF_8))
            }.getOrElse { throw FirmwarePackageException("invalid_manifest", it) }
            validateManifest(manifest, manifestBytes, imageLength)
            verifyManifestSignature(manifestBytes, signature)

            imageFile.parentFile?.mkdirs()
            val imageDigest = MessageDigest.getInstance("SHA-256")
            var remaining = imageLength
            val buffer = ByteArray(16 * 1024)
            FileOutputStream(imageFile, false).use { output ->
                while (remaining > 0) {
                    val count = input.read(buffer, 0, minOf(buffer.size, remaining))
                    if (count <= 0) fail("truncated_image")
                    imageDigest.update(buffer, 0, count)
                    output.write(buffer, 0, count)
                    remaining -= count
                }
                output.fd.sync()
            }
            if (input.read() != -1) fail("trailing_package_data")
            if (hex(imageDigest.digest()) != manifest.imageSha256) fail("image_hash_mismatch")
            validateStagedImage(imageFile, imageLength, manifest)
            return VerifiedFirmwarePackage(manifest, manifestBytes, signature, imageFile)
        } catch (failure: FirmwarePackageException) {
            imageFile.delete()
            throw failure
        } catch (failure: Throwable) {
            imageFile.delete()
            throw FirmwarePackageException("package_read_failed", failure)
        }
    }

    private fun validateManifest(
        value: FirmwareUpdateManifest,
        raw: ByteArray,
        headerImageBytes: Int,
    ) {
        val declaredImageLimit = maximumImageBytes(value.partitionBytes)
        if (value.schema != MANIFEST_SCHEMA ||
            !releaseId.matches(value.releaseId) ||
            !isSupportedFirmwareVersion(value.firmwareVersion) ||
            value.deviceClass != DEVICE_CLASS ||
            value.imageFormat != IMAGE_FORMAT ||
            value.imageBytes != headerImageBytes ||
            declaredImageLimit == null ||
            value.imageBytes !in 1..declaredImageLimit ||
            !digest.matches(value.imageSha256) ||
            value.chunkBytes != CHUNK_BYTES ||
            !value.rollback
        ) fail("unsupported_manifest")
        if (!raw.contentEquals(canonicalManifest(value).toByteArray(Charsets.UTF_8))) {
            fail("noncanonical_manifest")
        }
    }

    internal fun isSupportedFirmwareVersion(value: String): Boolean =
        parseBoundedSemVer(value) != null

    /**
     * The signed manifest names the physical OTA-slot size; Android never
     * derives or guesses a flash address. Both released A/B layouts remain
     * importable, while the target firmware independently requires the size
     * that matches its own partition table.
     */
    internal fun maximumImageBytes(partitionBytes: Int): Int? = when (partitionBytes) {
        LEGACY_PARTITION_BYTES, CURRENT_PARTITION_BYTES -> partitionBytes - JOURNAL_BYTES
        else -> null
    }

    internal fun isSupportedFirmwareLayout(partitionBytes: Int, imageBytes: Int): Boolean =
        maximumImageBytes(partitionBytes)?.let { imageBytes in 1..it } == true

    /**
     * Completes all local validation before the staged image can become a
     * [VerifiedFirmwarePackage]. A current-layout identity failure removes the
     * staged image here as well as at the outer package-reader boundary.
     */
    internal fun validateStagedImage(
        imageFile: File,
        declaredBytes: Int,
        manifest: FirmwareUpdateManifest,
    ) {
        try {
            validateEsp32S3Image(imageFile, declaredBytes)
            validateFirmwareIdentity(imageFile, manifest)
        } catch (failure: Throwable) {
            imageFile.delete()
            throw failure
        }
    }

    /**
     * Binds a signed current-layout manifest to the immutable identity compiled
     * into the ESP application. Legacy 3.3 MiB packages predate this record and
     * intentionally remain importable.
     */
    internal fun validateFirmwareIdentity(
        imageFile: File,
        manifest: FirmwareUpdateManifest,
    ) {
        when (manifest.partitionBytes) {
            LEGACY_PARTITION_BYTES -> return
            CURRENT_PARTITION_BYTES -> Unit
            else -> fail("unsupported_manifest")
        }
        if (manifest.imageBytes !in 1..CURRENT_MAXIMUM_IMAGE_BYTES ||
            imageFile.length() != manifest.imageBytes.toLong()
        ) {
            fail("invalid_firmware_identity")
        }

        // The signed manifest caps this allocation at the current slot's
        // application limit (just under 3 MiB).
        val image = imageFile.readBytes()
        if (image.size != manifest.imageBytes) fail("invalid_firmware_identity")
        var identityStart = -1
        var cursor = 0
        while (true) {
            val start = image.indexOfSequence(firmwareIdentityMagic, cursor)
            if (start < 0) break
            if (identityStart >= 0) fail("invalid_firmware_identity")
            identityStart = start
            cursor = start + 1
        }
        if (identityStart < 0) fail("invalid_firmware_identity")

        val start = identityStart
        val limit = minOf(image.size, start + FIRMWARE_IDENTITY_MAX_BYTES + 1)
        var terminator = -1
        for (index in start until limit) {
            if (image[index] == 0.toByte()) {
                terminator = index
                break
            }
        }
        if (terminator < 0) fail("invalid_firmware_identity")
        val raw = image.copyOfRange(start, terminator)
        if (raw.any { (it.toInt() and 0xff) > 0x7f }) fail("invalid_firmware_identity")

        val match = firmwareIdentity.matchEntire(raw.toString(Charsets.US_ASCII))
            ?: fail("invalid_firmware_identity")
        val groups = match.groupValues
        val schema = groups[1].toLongOrNull(10) ?: fail("invalid_firmware_identity")
        val length = groups[2].toIntOrNull(10) ?: fail("invalid_firmware_identity")
        val version = groups[3]
        val crcBoundary = raw.indexOfSequence(firmwareIdentityCrcField, 0)
        val expectedCrc = groups[22].toLongOrNull(16) ?: fail("invalid_firmware_identity")
        if (length != raw.size + 1 || crcBoundary < 0 ||
            CRC32().apply { update(raw, 0, crcBoundary) }.value != expectedCrc ||
            !isSupportedFirmwareVersion(version)
        ) {
            fail("invalid_firmware_identity")
        }

        fun hex(index: Int): Long =
            groups[index].toLongOrNull(16) ?: fail("invalid_firmware_identity")

        if (schema != FIRMWARE_IDENTITY_SCHEMA ||
            version != manifest.firmwareVersion ||
            groups[4] != FIRMWARE_IDENTITY_DEVICE_CLASS ||
            manifest.deviceClass != DEVICE_CLASS ||
            groups[5] != FIRMWARE_LAYOUT_ID ||
            hex(6) != FIRMWARE_FLASH_BYTES ||
            hex(7) != FIRMWARE_NVS_OFFSET ||
            hex(8) != FIRMWARE_NVS_BYTES ||
            hex(9) != FIRMWARE_OTA_DATA_OFFSET ||
            hex(10) != FIRMWARE_OTA_DATA_BYTES ||
            hex(11) != FIRMWARE_APP0_OFFSET ||
            hex(12) != FIRMWARE_APP1_OFFSET ||
            hex(13) != manifest.partitionBytes.toLong() ||
            hex(14) != JOURNAL_BYTES.toLong() ||
            hex(15) != CURRENT_MAXIMUM_IMAGE_BYTES.toLong() ||
            hex(16) != FIRMWARE_SPIFFS_OFFSET ||
            hex(17) != FIRMWARE_SPIFFS_BYTES ||
            hex(18) != FIRMWARE_CONNECTIVITY_OFFSET ||
            hex(19) != FIRMWARE_CONNECTIVITY_BYTES ||
            hex(20) != FIRMWARE_COREDUMP_OFFSET ||
            hex(21) != FIRMWARE_COREDUMP_BYTES
        ) {
            fail("firmware_identity_mismatch")
        }
    }

    /**
     * Firmware 0.20.3 is the serial-only migration boundary to the 3 MiB A/B
     * layout. The authenticated update-status receipt reports the running
     * firmware version, so a mismatched package can be rejected before begin.
     */
    internal fun isLayoutCompatibleWithDevice(
        firmwareVersion: String?,
        partitionBytes: Int,
    ): Boolean {
        val parsed = parseBoundedSemVer(firmwareVersion) ?: return false
        val baseComparison = compareValuesBy(
            parsed,
            BoundedSemVer(0L, 20L, 3L, false),
            { it.major },
            { it.minor },
            { it.patch },
        )
        // 0.20.3 prereleases precede the final serial-migration release and
        // therefore remain legacy. Build metadata does not change precedence.
        val migrated = baseComparison > 0 ||
            (baseComparison == 0 && !parsed.hasPrerelease)
        return partitionBytes == if (migrated) CURRENT_PARTITION_BYTES else LEGACY_PARTITION_BYTES
    }

    private fun parseBoundedSemVer(value: String?): BoundedSemVer? {
        if (value.isNullOrEmpty() || value.length > MAX_VERSION_BYTES ||
            value.any { it.code > 0x7f }
        ) {
            return null
        }

        val suffixStart = value.indexOfFirst { it == '-' || it == '+' }
        val coreEnd = suffixStart.takeIf { it >= 0 } ?: value.length
        val core = value.substring(0, coreEnd)
        val firstDot = core.indexOf('.')
        val secondDot = if (firstDot >= 0) core.indexOf('.', firstDot + 1) else -1
        if (firstDot <= 0 || secondDot <= firstDot + 1 ||
            secondDot == core.lastIndex || core.indexOf('.', secondDot + 1) >= 0
        ) {
            return null
        }
        val major = parseCoreNumber(core.substring(0, firstDot)) ?: return null
        val minor = parseCoreNumber(core.substring(firstDot + 1, secondDot)) ?: return null
        val patch = parseCoreNumber(core.substring(secondDot + 1)) ?: return null

        var hasPrerelease = false
        if (suffixStart >= 0) {
            when (value[suffixStart]) {
                '-' -> {
                    hasPrerelease = true
                    val buildStart = value.indexOf('+', suffixStart + 1)
                    val prereleaseEnd = buildStart.takeIf { it >= 0 } ?: value.length
                    if (!validIdentifiers(
                            value.substring(suffixStart + 1, prereleaseEnd),
                            rejectNumericLeadingZero = true,
                        )
                    ) {
                        return null
                    }
                    if (buildStart >= 0 && !validIdentifiers(
                            value.substring(buildStart + 1),
                            rejectNumericLeadingZero = false,
                        )
                    ) {
                        return null
                    }
                }
                '+' -> if (!validIdentifiers(
                        value.substring(suffixStart + 1),
                        rejectNumericLeadingZero = false,
                    )
                ) {
                    return null
                }
            }
        }
        return BoundedSemVer(major, minor, patch, hasPrerelease)
    }

    private fun parseCoreNumber(value: String): Long? {
        if (value.isEmpty() || value.any { it !in '0'..'9' } ||
            (value.length > 1 && value[0] == '0')
        ) {
            return null
        }
        return value.toLongOrNull()
    }

    private fun validIdentifiers(value: String, rejectNumericLeadingZero: Boolean): Boolean {
        if (value.isEmpty()) return false
        var start = 0
        while (start <= value.length) {
            val dot = value.indexOf('.', start)
            val end = dot.takeIf { it >= 0 } ?: value.length
            if (end == start) return false
            val identifier = value.substring(start, end)
            if (identifier.any { character ->
                    character !in '0'..'9' && character !in 'A'..'Z' &&
                        character !in 'a'..'z' && character != '-'
                }
            ) {
                return false
            }
            if (rejectNumericLeadingZero && identifier.length > 1 &&
                identifier[0] == '0' && identifier.all { it in '0'..'9' }
            ) {
                return false
            }
            if (dot < 0) return true
            start = dot + 1
        }
        return false
    }

    private fun verifyManifestSignature(manifest: ByteArray, signature: ByteArray) {
        val keyBytes = Base64.getDecoder().decode(UPDATE_AUTHORITY_SPKI_B64)
        if (sha256(keyBytes) != UPDATE_AUTHORITY_SPKI_SHA256) fail("update_authority_mismatch")
        val verified = try {
            verifyEd25519Spki(keyBytes, manifest, signature)
        } catch (failure: Throwable) {
            throw FirmwarePackageException("signature_verification_unavailable", failure)
        }
        if (!verified) fail("invalid_manifest_signature")
    }

    /** Validates the signed raw image using the public ESP32-S3 application-image contract. */
    internal fun validateEsp32S3Image(imageFile: File, declaredBytes: Int) {
        if (declaredBytes < ESP_HEADER_BYTES || imageFile.length() != declaredBytes.toLong()) {
            fail("invalid_esp_image")
        }
        RandomAccessFile(imageFile, "r").use { image ->
            val header = ByteArray(ESP_HEADER_BYTES)
            image.readFullyOrFail(header)
            val segmentCount = header[1].toInt() and 0xff
            if ((header[0].toInt() and 0xff) != ESP_IMAGE_MAGIC ||
                segmentCount !in 1..MAX_ESP_SEGMENTS ||
                header.u16le(12) != ESP32_S3_CHIP_ID ||
                (header[23].toInt() and 0xff) != 1
            ) fail("invalid_esp_image")

            var cursor = ESP_HEADER_BYTES.toLong()
            var checksum = ESP_CHECKSUM_SEED
            val buffer = ByteArray(16 * 1024)
            repeat(segmentCount) {
                if (cursor + ESP_SEGMENT_HEADER_BYTES > declaredBytes.toLong()) {
                    fail("invalid_esp_image")
                }
                image.seek(cursor)
                val segmentHeader = ByteArray(ESP_SEGMENT_HEADER_BYTES)
                image.readFullyOrFail(segmentHeader)
                val loadAddress = segmentHeader.u32le(0)
                val dataBytes = segmentHeader.u32le(4)
                cursor += ESP_SEGMENT_HEADER_BYTES
                if (dataBytes < 1L || dataBytes % 4L != 0L ||
                    loadAddress + dataBytes > UINT32_LIMIT ||
                    dataBytes > declaredBytes.toLong() - cursor
                ) fail("invalid_esp_image")

                image.seek(cursor)
                var remaining = dataBytes
                while (remaining > 0) {
                    val count = image.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                    if (count <= 0) fail("invalid_esp_image")
                    repeat(count) { index ->
                        checksum = checksum xor (buffer[index].toInt() and 0xff)
                    }
                    remaining -= count
                }
                cursor += dataBytes
            }

            val checksumOffset = cursor + (15L - (cursor % 16L))
            if (checksumOffset >= declaredBytes.toLong()) fail("invalid_esp_image")
            image.seek(checksumOffset)
            if (image.read() != checksum) fail("invalid_esp_image")

            val digestOffset = checksumOffset + 1L
            if (digestOffset + SHA256_BYTES != declaredBytes.toLong()) fail("invalid_esp_image")
            val digest = MessageDigest.getInstance("SHA-256")
            image.seek(0L)
            var remaining = digestOffset
            while (remaining > 0) {
                val count = image.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                if (count <= 0) fail("invalid_esp_image")
                digest.update(buffer, 0, count)
                remaining -= count
            }
            val appended = ByteArray(SHA256_BYTES)
            image.readFullyOrFail(appended)
            if (!MessageDigest.isEqual(digest.digest(), appended)) fail("invalid_esp_image")
        }
    }

    private fun canonicalManifest(value: FirmwareUpdateManifest): String =
        "{\"schema\":\"${value.schema}\",\"release_id\":\"${value.releaseId}\"," +
            "\"firmware_version\":\"${value.firmwareVersion}\",\"device_class\":\"${value.deviceClass}\"," +
            "\"image_format\":\"${value.imageFormat}\",\"image_bytes\":${value.imageBytes}," +
            "\"image_sha256\":\"${value.imageSha256}\",\"partition_bytes\":${value.partitionBytes}," +
            "\"chunk_bytes\":${value.chunkBytes},\"rollback\":true}"

    private fun InputStream.readExact(bytes: Int, code: String): ByteArray {
        val output = ByteArray(bytes)
        var offset = 0
        while (offset < bytes) {
            val count = read(output, offset, bytes - offset)
            if (count <= 0) fail(code)
            offset += count
        }
        return output
    }

    private fun ByteArray.u16(offset: Int): Int =
        ((this[offset].toInt() and 0xff) shl 8) or (this[offset + 1].toInt() and 0xff)

    private fun ByteArray.u16le(offset: Int): Int =
        (this[offset].toInt() and 0xff) or ((this[offset + 1].toInt() and 0xff) shl 8)

    private fun ByteArray.u32le(offset: Int): Long =
        (this[offset].toLong() and 0xff) or
            ((this[offset + 1].toLong() and 0xff) shl 8) or
            ((this[offset + 2].toLong() and 0xff) shl 16) or
            ((this[offset + 3].toLong() and 0xff) shl 24)

    private fun RandomAccessFile.readFullyOrFail(output: ByteArray) {
        try {
            readFully(output)
        } catch (_: EOFException) {
            fail("invalid_esp_image")
        }
    }

    private fun ByteArray.u32(offset: Int): Int {
        val value = ((this[offset].toLong() and 0xff) shl 24) or
            ((this[offset + 1].toLong() and 0xff) shl 16) or
            ((this[offset + 2].toLong() and 0xff) shl 8) or
            (this[offset + 3].toLong() and 0xff)
        if (value > Int.MAX_VALUE) fail("invalid_package_header")
        return value.toInt()
    }

    private fun ByteArray.indexOfSequence(sequence: ByteArray, fromIndex: Int): Int {
        if (sequence.isEmpty()) return fromIndex.coerceIn(0, size)
        val first = fromIndex.coerceAtLeast(0)
        val last = size - sequence.size
        outer@ for (start in first..last) {
            for (index in sequence.indices) {
                if (this[start + index] != sequence[index]) continue@outer
            }
            return start
        }
        return -1
    }

    private fun fail(code: String): Nothing = throw FirmwarePackageException(code)

    const val MANIFEST_SCHEMA = "kitsu.ble-firmware.v1"
    const val DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb"
    const val IMAGE_FORMAT = "esp32s3-app"
    const val LEGACY_PARTITION_BYTES = 0x330000
    const val CURRENT_PARTITION_BYTES = 0x300000
    const val JOURNAL_BYTES = 0x1000
    const val CHUNK_BYTES = 4_096
    const val MAX_SUPPORTED_IMAGE_BYTES = LEGACY_PARTITION_BYTES - JOURNAL_BYTES
    private const val CURRENT_MAXIMUM_IMAGE_BYTES = CURRENT_PARTITION_BYTES - JOURNAL_BYTES
    private const val FIRMWARE_IDENTITY_SCHEMA = 1L
    private const val FIRMWARE_IDENTITY_DEVICE_CLASS = "heltec-v3.2"
    private const val FIRMWARE_LAYOUT_ID = "kitsu-8m-dual-ota-3m-v1"
    private const val FIRMWARE_FLASH_BYTES = 0x800000L
    private const val FIRMWARE_NVS_OFFSET = 0x009000L
    private const val FIRMWARE_NVS_BYTES = 0x040000L
    private const val FIRMWARE_OTA_DATA_OFFSET = 0x049000L
    private const val FIRMWARE_OTA_DATA_BYTES = 0x002000L
    private const val FIRMWARE_APP0_OFFSET = 0x050000L
    private const val FIRMWARE_APP1_OFFSET = 0x350000L
    private const val FIRMWARE_SPIFFS_OFFSET = 0x670000L
    private const val FIRMWARE_SPIFFS_BYTES = 0x140000L
    private const val FIRMWARE_CONNECTIVITY_OFFSET = 0x7b0000L
    private const val FIRMWARE_CONNECTIVITY_BYTES = 0x040000L
    private const val FIRMWARE_COREDUMP_OFFSET = 0x7f0000L
    private const val FIRMWARE_COREDUMP_BYTES = 0x010000L
    private const val FIRMWARE_IDENTITY_MAX_BYTES = 384
    private const val MAX_VERSION_BYTES = 32
    private const val HEADER_BYTES = 20
    private const val MAX_MANIFEST_BYTES = 1_024
    private const val SIGNATURE_BYTES = 64
    private const val ESP_IMAGE_MAGIC = 0xE9
    private const val ESP_HEADER_BYTES = 24
    private const val ESP_SEGMENT_HEADER_BYTES = 8
    private const val MAX_ESP_SEGMENTS = 16
    private const val ESP32_S3_CHIP_ID = 9
    private const val ESP_CHECKSUM_SEED = 0xEF
    private const val SHA256_BYTES = 32
    private const val UINT32_LIMIT = 0x1_0000_0000L
    private const val UPDATE_AUTHORITY_SPKI_B64 =
        "MCowBQYDK2VwAyEAJAAR8Unpz7n7h/q02cpFc8HH/7OHF3ZYAAXsQa7lE4I="
    private const val UPDATE_AUTHORITY_SPKI_SHA256 =
        "df530766fbc4fc93e82cdbd354ebe4a17a453c83e9bb7fe2af30ca2d202494ab"
    private val firmwareIdentityMagic = "KITSU-ID1|".toByteArray(Charsets.US_ASCII)
    private val firmwareIdentityCrcField = "|crc32=".toByteArray(Charsets.US_ASCII)
    private val firmwareIdentity = Regex(
        "^KITSU-ID1\\|schema=([0-9]+)\\|length=([0-9]{4})\\|version=([^|]+)" +
            "\\|device_class=([^|]+)\\|layout=([^|]+)\\|flash=([0-9a-f]{8})" +
            "\\|nvs=([0-9a-f]{8})/([0-9a-f]{8})" +
            "\\|otadata=([0-9a-f]{8})/([0-9a-f]{8})" +
            "\\|app0=([0-9a-f]{8})\\|app1=([0-9a-f]{8})" +
            "\\|slot=([0-9a-f]{8})\\|journal=([0-9a-f]{8})" +
            "\\|max=([0-9a-f]{8})" +
            "\\|spiffs=([0-9a-f]{8})/([0-9a-f]{8})" +
            "\\|conn=([0-9a-f]{8})/([0-9a-f]{8})" +
            "\\|coredump=([0-9a-f]{8})/([0-9a-f]{8})" +
            "\\|crc32=([0-9a-f]{8})\\|end$",
    )
}

internal fun verifyEd25519Spki(spki: ByteArray, message: ByteArray, signature: ByteArray): Boolean {
    require(spki.size >= ED25519_PUBLIC_KEY_BYTES)
    require(signature.size == 64)
    val curve = EdDSANamedCurveTable.getByName("Ed25519")
        ?: throw IllegalStateException("ed25519_curve_unavailable")
    val rawPublicKey = spki.copyOfRange(spki.size - ED25519_PUBLIC_KEY_BYTES, spki.size)
    val key = EdDSAPublicKey(EdDSAPublicKeySpec(rawPublicKey, curve))
    return EdDSAEngine(MessageDigest.getInstance(curve.hashAlgorithm)).run {
        initVerify(key)
        update(message)
        verify(signature)
    }
}

internal fun sha256(bytes: ByteArray): String = hex(MessageDigest.getInstance("SHA-256").digest(bytes))

private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
private const val ED25519_PUBLIC_KEY_BYTES = 32
