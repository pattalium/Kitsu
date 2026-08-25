package ptl.kitsu.app.security

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.model.EncounterUnlockCode

interface EncounterCodeVault {
    fun read(): List<EncounterUnlockCode>
    fun upsert(values: List<EncounterUnlockCode>): List<EncounterUnlockCode>
    fun deleteForDevice(deviceId: String): List<EncounterUnlockCode>
    fun updateState(
        deviceId: String,
        codeId: String,
        redeemed: Boolean? = null,
        installed: Boolean? = null,
    ): List<EncounterUnlockCode>
}

class EncounterCodeVaultException(val code: String, cause: Throwable? = null) : Exception(code, cause)

@Serializable
private data class EncounterVaultSnapshot(
    val schema: String = VAULT_SCHEMA,
    val records: List<EncounterUnlockCode> = emptyList(),
)

/**
 * A dedicated, multi-device vault. It is intentionally independent from the selected-device
 * owner cache and controller authorization lifecycle.
 */
class AndroidKeystoreEncounterCodeVault(context: Context) : EncounterCodeVault {
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }
    private val file = File(context.filesDir, FILE_NAME)

    @Synchronized
    override fun read(): List<EncounterUnlockCode> = readLocked()

    @Synchronized
    override fun upsert(values: List<EncounterUnlockCode>): List<EncounterUnlockCode> {
        val merged = EncounterVaultPolicy.merge(readLocked(), values)
        writeLocked(merged)
        return merged
    }

    @Synchronized
    override fun deleteForDevice(deviceId: String): List<EncounterUnlockCode> {
        require(EncounterCodePolicy.validDeviceId(deviceId)) { "invalid_encounter_device_id" }
        val retained = EncounterVaultPolicy.deleteForDevice(readLocked(), deviceId)
        writeLocked(retained)
        return retained
    }

    @Synchronized
    override fun updateState(
        deviceId: String,
        codeId: String,
        redeemed: Boolean?,
        installed: Boolean?,
    ): List<EncounterUnlockCode> {
        val current = readLocked()
        val updated = EncounterVaultPolicy.updateState(current, deviceId, codeId, redeemed, installed)
        writeLocked(updated)
        return updated
    }

    private fun readLocked(): List<EncounterUnlockCode> {
        if (!file.exists()) return emptyList()
        return try {
            val payload = file.readBytes()
            if (payload.size <= HEADER_BYTES + IV_BYTES) throw IllegalStateException("vault_truncated")
            if (!payload.copyOfRange(0, HEADER_BYTES).contentEquals(FILE_HEADER)) {
                throw IllegalStateException("vault_header_invalid")
            }
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(
                Cipher.DECRYPT_MODE,
                secretKey(),
                GCMParameterSpec(
                    128,
                    payload.copyOfRange(HEADER_BYTES, HEADER_BYTES + IV_BYTES),
                ),
            )
            cipher.updateAAD(FILE_HEADER)
            val plaintext = cipher.doFinal(payload.copyOfRange(HEADER_BYTES + IV_BYTES, payload.size))
            try {
                val snapshot = json.decodeFromString(
                    EncounterVaultSnapshot.serializer(),
                    plaintext.toString(Charsets.UTF_8),
                )
                if (snapshot.schema != VAULT_SCHEMA ||
                    snapshot.records.any { EncounterCodePolicy.validationError(it) != null } ||
                    snapshot.records.map(EncounterUnlockCode::vaultKey).distinct().size != snapshot.records.size
                ) throw IllegalStateException("vault_payload_invalid")
                EncounterVaultPolicy.merge(emptyList(), snapshot.records)
            } finally {
                plaintext.fill(0)
                payload.fill(0)
            }
        } catch (failure: EncounterCodeVaultException) {
            throw failure
        } catch (failure: Throwable) {
            throw EncounterCodeVaultException("encounter_vault_read_failed", failure)
        }
    }

    private fun writeLocked(records: List<EncounterUnlockCode>) {
        if (records.isEmpty()) {
            if (file.exists() && !file.delete()) {
                throw EncounterCodeVaultException("encounter_vault_delete_failed")
            }
            return
        }
        val normalized = EncounterVaultPolicy.merge(emptyList(), records)
        val plaintext = json.encodeToString(
            EncounterVaultSnapshot.serializer(),
            EncounterVaultSnapshot(records = normalized),
        ).toByteArray(Charsets.UTF_8)
        if (plaintext.size > EncounterCodePolicy.MAX_VAULT_PLAINTEXT_BYTES) {
            plaintext.fill(0)
            throw EncounterCodeVaultException("encounter_vault_too_large")
        }
        try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, secretKey())
            cipher.updateAAD(FILE_HEADER)
            val ciphertext = cipher.doFinal(plaintext)
            val temporary = File(file.parentFile, "${file.name}.tmp")
            temporary.outputStream().use { output ->
                output.write(FILE_HEADER)
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
        } catch (failure: EncounterCodeVaultException) {
            throw failure
        } catch (failure: Throwable) {
            throw EncounterCodeVaultException("encounter_vault_write_failed", failure)
        } finally {
            plaintext.fill(0)
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
        const val FILE_NAME = "encounter-code-vault-v1.bin"
        const val KEY_ALIAS = "kitsu.mobile.encounter-code-vault.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        val FILE_HEADER = byteArrayOf('K'.code.toByte(), 'V'.code.toByte(), 'C'.code.toByte(), 1)
        const val HEADER_BYTES = 4
        const val IV_BYTES = 12
    }
}

internal object EncounterVaultPolicy {
    fun merge(
        current: List<EncounterUnlockCode>,
        incoming: List<EncounterUnlockCode>,
    ): List<EncounterUnlockCode> {
        val byKey = linkedMapOf<String, EncounterUnlockCode>()
        current.forEach { value -> putValid(byKey, value) }
        incoming.forEach { value ->
            EncounterCodePolicy.validationError(value)?.let { throw IllegalArgumentException(it) }
            val old = byKey[value.vaultKey]
            byKey[value.vaultKey] = if (old == null) normalizeState(value) else normalizeState(
                value.copy(
                    packId = value.packId ?: old.packId,
                    creatureName = value.creatureName ?: old.creatureName,
                    source = value.source ?: old.source,
                    acquiredAtEpoch = maxOf(old.acquiredAtEpoch, value.acquiredAtEpoch),
                    redeemed = old.redeemed || value.redeemed,
                    installed = old.installed || value.installed,
                ),
            )
        }
        return byKey.values
            .sortedWith(
                compareByDescending<EncounterUnlockCode> { it.acquiredAtEpoch }
                    .thenBy(EncounterUnlockCode::deviceId)
                    .thenBy(EncounterUnlockCode::codeId),
            )
            .take(EncounterCodePolicy.MAX_VAULT_RECORDS)
    }

    fun deleteForDevice(
        current: List<EncounterUnlockCode>,
        deviceId: String,
    ): List<EncounterUnlockCode> = current.filterNot { it.deviceId == deviceId }

    fun updateState(
        current: List<EncounterUnlockCode>,
        deviceId: String,
        codeId: String,
        redeemed: Boolean?,
        installed: Boolean?,
    ): List<EncounterUnlockCode> {
        require(EncounterCodePolicy.validDeviceId(deviceId)) { "invalid_encounter_device_id" }
        var found = false
        val updated = current.map { value ->
            if (value.deviceId == deviceId && value.codeId == codeId) {
                found = true
                normalizeState(
                    value.copy(
                        redeemed = redeemed ?: value.redeemed,
                        installed = installed ?: value.installed,
                    ),
                )
            } else value
        }
        require(found) { "encounter_code_not_found" }
        return merge(emptyList(), updated)
    }

    private fun putValid(
        target: MutableMap<String, EncounterUnlockCode>,
        value: EncounterUnlockCode,
    ) {
        EncounterCodePolicy.validationError(value)?.let { throw IllegalArgumentException(it) }
        target[value.vaultKey] = normalizeState(value)
    }

    private fun normalizeState(value: EncounterUnlockCode): EncounterUnlockCode =
        if (value.installed && !value.redeemed) value.copy(redeemed = true) else value
}

private const val VAULT_SCHEMA = "kitsu.android.encounter-code-vault.v1"
