package app.kitsu.mobile.cache

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import app.kitsu.mobile.model.HistoryEntry
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.Message
import app.kitsu.mobile.model.Peer
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

@Serializable
data class CacheSnapshot(
    val status: KitsuStatus? = null,
    val history: List<HistoryEntry> = emptyList(),
    val peers: List<Peer> = emptyList(),
    val messages: List<Message> = emptyList(),
    val historyCursor: String? = null,
    val messageCursor: String? = null,
    val writtenAt: Long,
)

object CachePolicy {
    const val MAX_HISTORY = 256
    const val MAX_PEERS = 256
    const val MAX_MESSAGES = 256
    const val MAX_PLAINTEXT_BYTES = 512 * 1024

    fun bounded(snapshot: CacheSnapshot, json: Json = Json): CacheSnapshot {
        var candidate = snapshot.copy(
            history = snapshot.history.takeLast(MAX_HISTORY),
            peers = snapshot.peers.takeLast(MAX_PEERS),
            messages = snapshot.messages.takeLast(MAX_MESSAGES),
        )
        while (json.encodeToString(CacheSnapshot.serializer(), candidate).toByteArray().size > MAX_PLAINTEXT_BYTES) {
            candidate = when {
                candidate.history.size >= candidate.messages.size &&
                    candidate.history.size >= candidate.peers.size && candidate.history.isNotEmpty() ->
                    candidate.copy(history = candidate.history.drop(1))
                candidate.peers.size >= candidate.messages.size && candidate.peers.isNotEmpty() ->
                    candidate.copy(peers = candidate.peers.drop(1))
                candidate.messages.isNotEmpty() -> candidate.copy(messages = candidate.messages.drop(1))
                candidate.status != null -> candidate.copy(status = null)
                else -> return candidate
            }
        }
        return candidate
    }
}

class EncryptedBoundedCache(context: Context) {
    private val json = Json { ignoreUnknownKeys = true }
    private val file = File(context.filesDir, "owner-cache-v1.bin")

    fun write(snapshot: CacheSnapshot) {
        val plaintext = json.encodeToString(CacheSnapshot.serializer(), CachePolicy.bounded(snapshot, json))
            .toByteArray(Charsets.UTF_8)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        val ciphertext = cipher.doFinal(plaintext)
        val temporary = File(file.parentFile, "${file.name}.tmp")
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

    fun read(): CacheSnapshot? = runCatching {
        if (!file.exists() || file.length() <= IV_BYTES) return null
        val payload = file.readBytes()
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(
            Cipher.DECRYPT_MODE,
            secretKey(),
            GCMParameterSpec(128, payload.copyOfRange(0, IV_BYTES)),
        )
        val plaintext = cipher.doFinal(payload.copyOfRange(IV_BYTES, payload.size))
        json.decodeFromString(CacheSnapshot.serializer(), plaintext.toString(Charsets.UTF_8))
    }.getOrNull()

    fun clear() {
        if (file.exists() && !file.delete()) throw IllegalStateException("cache_clear_failed")
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

    companion object {
        private const val KEY_ALIAS = "kitsu.mobile.cache.v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val IV_BYTES = 12
    }
}
