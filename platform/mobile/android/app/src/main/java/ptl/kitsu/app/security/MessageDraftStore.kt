package ptl.kitsu.app.security

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.KeyStore
import java.util.Locale
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import ptl.kitsu.app.model.MAX_MESSAGE_BYTES
import ptl.kitsu.app.model.MeshPeerKeyPolicy

@Serializable
data class MessageDraftRecord(
    val deviceAddress: String,
    val threadKey: String,
    val text: String,
    val updatedAtMillis: Long,
) {
    val bindingKey: String
        get() = "${MessageDraftPolicy.normalizeDeviceAddress(deviceAddress)}\u0000$threadKey"
}

@Serializable
private data class MessageDraftSnapshot(
    val schema: String = MessageDraftPolicy.SCHEMA,
    val records: List<MessageDraftRecord> = emptyList(),
)

object MessageDraftPolicy {
    const val SCHEMA = "kitsu.message-drafts.v1"
    const val MAX_RECORDS = 96
    const val MAX_RECORDS_PER_DEVICE = 32
    const val MAX_PLAINTEXT_BYTES = 64 * 1024

    private const val DIRECT_PREFIX = "direct:"
    private const val CHANNEL_PREFIX = "channel:"

    fun normalizeDeviceAddress(value: String): String = value.trim().uppercase(Locale.ROOT)

    fun validDeviceAddress(value: String): Boolean {
        val normalized = normalizeDeviceAddress(value)
        return normalized.isNotEmpty() && normalized.toByteArray(Charsets.UTF_8).size <= 64 &&
            normalized.none(Char::isISOControl)
    }

    fun validThreadKey(value: String): Boolean = when {
        value.startsWith(DIRECT_PREFIX) ->
            MeshPeerKeyPolicy.isCanonicalBase64Url(value.removePrefix(DIRECT_PREFIX))
        value.startsWith(CHANNEL_PREFIX) ->
            value.removePrefix(CHANNEL_PREFIX).toIntOrNull() in 0..3
        else -> false
    }

    fun validText(value: String): Boolean = value.isNotEmpty() &&
        value.toByteArray(Charsets.UTF_8).size <= MAX_MESSAGE_BYTES

    fun valid(record: MessageDraftRecord): Boolean =
        validDeviceAddress(record.deviceAddress) && validThreadKey(record.threadKey) &&
            validText(record.text) && record.updatedAtMillis >= 0L

    fun forDevice(records: List<MessageDraftRecord>, deviceAddress: String?): Map<String, String> {
        if (deviceAddress == null || !validDeviceAddress(deviceAddress)) return emptyMap()
        val normalized = normalizeDeviceAddress(deviceAddress)
        return records.asSequence()
            .filter(::valid)
            .filter { normalizeDeviceAddress(it.deviceAddress) == normalized }
            .sortedBy(MessageDraftRecord::updatedAtMillis)
            .associate { it.threadKey to it.text }
    }

    fun upsert(
        records: List<MessageDraftRecord>,
        deviceAddress: String,
        threadKey: String,
        text: String,
        updatedAtMillis: Long,
    ): List<MessageDraftRecord> {
        require(validDeviceAddress(deviceAddress)) { "invalid_draft_device" }
        require(validThreadKey(threadKey)) { "invalid_draft_thread" }
        require(text.isEmpty() || validText(text)) { "invalid_draft_text" }
        require(updatedAtMillis >= 0L) { "invalid_draft_time" }
        val normalized = normalizeDeviceAddress(deviceAddress)
        val retained = records.filterNot {
            normalizeDeviceAddress(it.deviceAddress) == normalized && it.threadKey == threadKey
        }
        if (text.isEmpty()) return bounded(retained)
        return bounded(
            retained + MessageDraftRecord(normalized, threadKey, text, updatedAtMillis),
        )
    }

    fun bounded(records: List<MessageDraftRecord>, json: Json = Json): List<MessageDraftRecord> {
        val latestByBinding = linkedMapOf<String, MessageDraftRecord>()
        records.asSequence().filter(::valid).sortedBy(MessageDraftRecord::updatedAtMillis).forEach {
            latestByBinding[it.bindingKey] = it.copy(
                deviceAddress = normalizeDeviceAddress(it.deviceAddress),
            )
        }
        val perDevice = latestByBinding.values.groupBy {
            normalizeDeviceAddress(it.deviceAddress)
        }.values.flatMap { deviceRecords ->
            deviceRecords.sortedBy(MessageDraftRecord::updatedAtMillis).takeLast(MAX_RECORDS_PER_DEVICE)
        }
        var candidate = perDevice.sortedBy(MessageDraftRecord::updatedAtMillis).takeLast(MAX_RECORDS)
        while (encodedBytes(candidate, json) > MAX_PLAINTEXT_BYTES && candidate.isNotEmpty()) {
            candidate = candidate.drop(1)
        }
        return candidate
    }

    private fun encodedBytes(records: List<MessageDraftRecord>, json: Json): Int =
        json.encodeToString(
            MessageDraftSnapshot.serializer(),
            MessageDraftSnapshot(records = records),
        ).toByteArray(Charsets.UTF_8).size
}

interface MessageDraftStore {
    suspend fun read(): List<MessageDraftRecord>
    suspend fun write(records: List<MessageDraftRecord>)
}

class AndroidKeystoreMessageDraftStore(context: Context) : MessageDraftStore {
    private val json = Json {
        encodeDefaults = true
        ignoreUnknownKeys = false
    }
    private val file = File(context.filesDir, FILE_NAME)

    override suspend fun read(): List<MessageDraftRecord> = runCatching {
        if (!file.exists() || file.length() <= IV_BYTES) return emptyList()
        val payload = file.readBytes()
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(
            Cipher.DECRYPT_MODE,
            secretKey(),
            GCMParameterSpec(128, payload.copyOfRange(0, IV_BYTES)),
        )
        val plaintext = cipher.doFinal(payload.copyOfRange(IV_BYTES, payload.size))
        val snapshot = json.decodeFromString(
            MessageDraftSnapshot.serializer(),
            plaintext.toString(Charsets.UTF_8),
        )
        check(snapshot.schema == MessageDraftPolicy.SCHEMA) { "invalid_draft_schema" }
        check(snapshot.records.all(MessageDraftPolicy::valid)) { "invalid_draft_store" }
        MessageDraftPolicy.bounded(snapshot.records, json)
    }.getOrElse {
        SafeLog.warn("message_drafts", "message_draft_read_failed", it)
        emptyList()
    }

    override suspend fun write(records: List<MessageDraftRecord>) {
        val bounded = MessageDraftPolicy.bounded(records, json)
        val plaintext = json.encodeToString(
            MessageDraftSnapshot.serializer(),
            MessageDraftSnapshot(records = bounded),
        ).toByteArray(Charsets.UTF_8)
        check(plaintext.size <= MessageDraftPolicy.MAX_PLAINTEXT_BYTES) { "draft_store_too_large" }
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        val ciphertext = cipher.doFinal(plaintext)
        val temporary = File(file.parentFile, "$FILE_NAME.tmp")
        temporary.outputStream().use { output ->
            output.write(cipher.iv)
            output.write(ciphertext)
            output.fd.sync()
        }
        try {
            Files.move(
                temporary.toPath(),
                file.toPath(),
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
        }
    }

    private fun secretKey(): SecretKey {
        val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(
                KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build(),
            )
            generateKey()
        }
    }

    private companion object {
        const val FILE_NAME = "message-drafts-v1.bin"
        const val KEY_ALIAS = "kitsu.mobile.message-drafts.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_BYTES = 12
    }
}
