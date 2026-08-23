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
    private val version = Regex(
        "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:[-+][0-9A-Za-z.-]+)?$",
    )
    private val digest = Regex("^[0-9a-f]{64}$")

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
            if (imageLength !in 1..MAX_IMAGE_BYTES) fail("invalid_image_length")

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
            validateEsp32S3Image(imageFile, imageLength)
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
        if (value.schema != MANIFEST_SCHEMA ||
            !releaseId.matches(value.releaseId) ||
            !isSupportedFirmwareVersion(value.firmwareVersion) ||
            value.deviceClass != DEVICE_CLASS ||
            value.imageFormat != IMAGE_FORMAT ||
            value.imageBytes != headerImageBytes ||
            value.imageBytes !in 1..MAX_IMAGE_BYTES ||
            !digest.matches(value.imageSha256) ||
            value.partitionBytes != PARTITION_BYTES ||
            value.chunkBytes != CHUNK_BYTES ||
            !value.rollback
        ) fail("unsupported_manifest")
        if (!raw.contentEquals(canonicalManifest(value).toByteArray(Charsets.UTF_8))) {
            fail("noncanonical_manifest")
        }
    }

    internal fun isSupportedFirmwareVersion(value: String): Boolean =
        version.matches(value) && value.toByteArray(Charsets.US_ASCII).size in 1..32

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

    private fun fail(code: String): Nothing = throw FirmwarePackageException(code)

    const val MANIFEST_SCHEMA = "kitsu.ble-firmware.v1"
    const val DEVICE_CLASS = "heltec-wifi-lora-32-v3-esp32s3-8mb"
    const val IMAGE_FORMAT = "esp32s3-app"
    const val PARTITION_BYTES = 3_342_336
    const val CHUNK_BYTES = 4_096
    const val MAX_IMAGE_BYTES = 0x32F000
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
