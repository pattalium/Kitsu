package ptl.kitsu.app.security

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

const val MAX_SAVED_KITSU = 3

@Serializable
data class BondedCompanion(
    val deviceAddress: String,
    val displayName: String,
    /** Unpadded base64url 16-byte lookup ID. */
    val controllerIdB64: String,
    /** Unpadded base64url 32-byte controller root, protected at rest by Keystore. */
    val controllerRootB64: String,
)

interface CredentialStore {
    suspend fun bondedCompanion(): BondedCompanion?
    suspend fun bondedCompanions(): List<BondedCompanion>
    suspend fun saveBondedCompanion(value: BondedCompanion?)
    suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion?
    suspend fun removeBondedCompanion(deviceAddress: String): Boolean
    suspend fun pendingBondedCompanion(): BondedCompanion?
    suspend fun savePendingBondedCompanion(value: BondedCompanion?)
    suspend fun pendingControllerForgetAddress(): String?
    suspend fun savePendingControllerForgetAddress(deviceAddress: String?)
}

class AndroidKeystoreCredentialStore(context: Context) : CredentialStore {
    private val prefs = context.getSharedPreferences("kitsu_encrypted_credentials", Context.MODE_PRIVATE)
    private val json = Json { ignoreUnknownKeys = true }

    init {
        // Retire version-1 credentials without touching controller roots, packs, or the
        // Android Bluetooth bond.
        prefs.edit().remove("oauth").remove("mobile_relay").commit()
        context.deleteSharedPreferences("kitsu_owner_selection")
    }

    override suspend fun bondedCompanion(): BondedCompanion? =
        read(ACTIVE)?.let { json.decodeFromString(BondedCompanion.serializer(), it) }

    override suspend fun bondedCompanions(): List<BondedCompanion> {
        val stored = read(ALL)?.let {
            json.decodeFromString(ListSerializer(BondedCompanion.serializer()), it)
        }
        return (stored ?: listOfNotNull(bondedCompanion()))
            .distinctBy { it.deviceAddress.uppercase() }
            .take(MAX_SAVED_KITSU)
    }

    override suspend fun saveBondedCompanion(value: BondedCompanion?) {
        if (value == null) {
            write(ACTIVE, null)
            write(ALL, null)
            return
        }
        val current = bondedCompanions()
        val replacing = current.any {
            it.deviceAddress.equals(value.deviceAddress, ignoreCase = true) ||
                it.controllerIdB64 == value.controllerIdB64
        }
        check(replacing || current.size < MAX_SAVED_KITSU) { "controller_device_limit" }
        val updated = current.filterNot {
            it.deviceAddress.equals(value.deviceAddress, ignoreCase = true) ||
                it.controllerIdB64 == value.controllerIdB64
        } + value
        write(ACTIVE, json.encodeToString(BondedCompanion.serializer(), value))
        write(ALL, json.encodeToString(ListSerializer(BondedCompanion.serializer()), updated))
    }

    override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? {
        val selected = bondedCompanions().firstOrNull {
            it.deviceAddress.equals(deviceAddress, ignoreCase = true)
        } ?: return null
        write(ACTIVE, json.encodeToString(BondedCompanion.serializer(), selected))
        return selected
    }

    override suspend fun removeBondedCompanion(deviceAddress: String): Boolean {
        val current = bondedCompanions()
        val updated = current.filterNot { it.deviceAddress.equals(deviceAddress, ignoreCase = true) }
        if (updated.size == current.size) return false
        val active = bondedCompanion()
        if (active?.deviceAddress.equals(deviceAddress, ignoreCase = true)) {
            write(ACTIVE, updated.firstOrNull()?.let {
                json.encodeToString(BondedCompanion.serializer(), it)
            })
        }
        // Retire the active pointer first. If a process stop interrupts the following
        // list write, the old entry remains recoverable for a pending authenticated Forget.
        write(ALL, updated.takeIf { it.isNotEmpty() }?.let {
            json.encodeToString(ListSerializer(BondedCompanion.serializer()), it)
        })
        return true
    }

    override suspend fun pendingBondedCompanion(): BondedCompanion? = read(PENDING_BOND)?.let {
        json.decodeFromString(BondedCompanion.serializer(), it)
    }

    override suspend fun savePendingBondedCompanion(value: BondedCompanion?) {
        write(PENDING_BOND, value?.let { json.encodeToString(BondedCompanion.serializer(), it) })
    }

    override suspend fun pendingControllerForgetAddress(): String? = read(PENDING_FORGET)

    override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) {
        write(PENDING_FORGET, deviceAddress)
    }

    private fun write(name: String, plaintext: String?) {
        if (plaintext == null) {
            check(prefs.edit().remove(name).commit()) { "credential_delete_failed" }
            return
        }
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        val encrypted = cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))
        val payload = Base64.encodeToString(cipher.iv + encrypted, Base64.NO_WRAP)
        check(prefs.edit().putString(name, payload).commit()) { "credential_write_failed" }
    }

    private fun read(name: String): String? {
        val payload = prefs.getString(name, null) ?: return null
        return try {
            val decoded = Base64.decode(payload, Base64.NO_WRAP)
            require(decoded.size > IV_BYTES)
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(
                Cipher.DECRYPT_MODE,
                secretKey(),
                GCMParameterSpec(128, decoded.copyOfRange(0, IV_BYTES)),
            )
            cipher.doFinal(decoded.copyOfRange(IV_BYTES, decoded.size)).toString(Charsets.UTF_8)
        } catch (failure: Throwable) {
            throw IllegalStateException("credential_decrypt_failed", failure)
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
        const val ACTIVE = "bonded"
        const val ALL = "bonded_all"
        const val PENDING_BOND = "bonded_pending"
        const val PENDING_FORGET = "controller_forget_pending"
        const val KEY_ALIAS = "kitsu.mobile.credentials.v1"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_BYTES = 12
    }
}
