package ptl.kitsu.app.cache

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.EncounterDiscoveryPolicy
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.Peer
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
    val channels: List<MeshChannel> = emptyList(),
    val messages: List<Message> = emptyList(),
    val historyCursor: String? = null,
    val messageCursor: String? = null,
    val writtenAt: Long,
    val deviceAddress: String? = null,
    val encounterDiscoveryDeviceId: String? = null,
    val encounterDiscovery: List<EncounterDiscoveryRecord> = emptyList(),
)

internal data class CachedEncounterDiscovery(
    val deviceId: String,
    val records: List<EncounterDiscoveryRecord>,
)

/** Device-derived rows are usable only for the credential that wrote the snapshot. */
internal object OwnerCacheBindingPolicy {
    fun restore(snapshot: CacheSnapshot?, activeDeviceAddress: String?): CacheSnapshot? {
        val address = activeDeviceAddress ?: return null
        return snapshot?.takeIf { cached ->
            cached.deviceAddress?.equals(address, ignoreCase = true) == true
        }
    }
}

/** Prevents an encrypted snapshot for one saved Kitsu from populating another's Guide. */
internal object EncounterDiscoveryCachePolicy {
    fun restore(
        snapshot: CacheSnapshot?,
        activeDeviceAddress: String?,
    ): CachedEncounterDiscovery? {
        snapshot ?: return null
        val address = activeDeviceAddress ?: return null
        if (!snapshot.deviceAddress.equals(address, ignoreCase = true)) return null
        val deviceId = snapshot.encounterDiscoveryDeviceId ?: return null
        if (snapshot.status?.deviceId != deviceId ||
            !EncounterDiscoveryPolicy.isExactPublicDiscovery(snapshot.encounterDiscovery)
        ) return null
        return CachedEncounterDiscovery(deviceId, snapshot.encounterDiscovery)
    }
}

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

interface OwnerCache {
    fun write(snapshot: CacheSnapshot)
    fun read(): CacheSnapshot?
    fun clear()
}

class EncryptedBoundedCache(context: Context) : OwnerCache {
    private val json = Json { ignoreUnknownKeys = true }
    private val file = File(context.filesDir, "local-cache-v2.bin")

    init {
        // Version 2 deliberately starts a new local-device namespace.
        File(context.filesDir, "owner-cache-v1.bin").delete()
    }

    override fun write(snapshot: CacheSnapshot) {
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

    override fun read(): CacheSnapshot? = runCatching {
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

    override fun clear() {
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
        private const val KEY_ALIAS = "kitsu.mobile.cache.v2"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val IV_BYTES = 12
    }
}
