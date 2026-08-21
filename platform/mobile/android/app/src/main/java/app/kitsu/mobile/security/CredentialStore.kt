package app.kitsu.mobile.security

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import app.kitsu.mobile.relay.MAX_MOBILE_RELAY_DEVICES
import app.kitsu.mobile.relay.MobileRelayBondPolicy
import app.kitsu.mobile.relay.MobileRelaySettings
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

@Serializable
data class BondedCompanion(
    val deviceAddress: String,
    val displayName: String,
    /** Unpadded base64url 16-byte lookup ID. */
    val controllerIdB64: String,
    /** Unpadded base64url 32-byte controller root, protected at rest by Keystore. */
    val controllerRootB64: String,
)

@Serializable
data class OAuthTokens(
    val accessToken: String,
    val refreshToken: String? = null,
    val accessTokenExpiresAtEpochSeconds: Long,
    val idToken: String? = null,
)

interface CredentialStore {
    suspend fun bondedCompanion(): BondedCompanion?
    suspend fun saveBondedCompanion(value: BondedCompanion?)
    suspend fun savePendingBondedCompanion(value: BondedCompanion?)
    suspend fun oauthTokens(): OAuthTokens?
    suspend fun saveOauthTokens(value: OAuthTokens?)

    /** Backward-compatible multi-device view; older stores still expose their active bond. */
    suspend fun bondedCompanions(): List<BondedCompanion> = listOfNotNull(bondedCompanion())

    suspend fun mobileRelaySettings(): MobileRelaySettings? = null

    suspend fun saveMobileRelaySettings(value: MobileRelaySettings?) = Unit
}

class AndroidKeystoreCredentialStore(context: Context) : CredentialStore {
    private val prefs = context.getSharedPreferences("kitsu_encrypted_credentials", Context.MODE_PRIVATE)
    private val json = Json { ignoreUnknownKeys = true }

    override suspend fun bondedCompanion(): BondedCompanion? =
        read("bonded")?.let { json.decodeFromString(BondedCompanion.serializer(), it) }

    override suspend fun saveBondedCompanion(value: BondedCompanion?) {
        if (value == null) {
            write("bonded", null)
            write("bonded_all", null)
            return
        }
        val companions = MobileRelayBondPolicy.upsert(bondedCompanions(), value)
        write("bonded", json.encodeToString(BondedCompanion.serializer(), value))
        write(
            "bonded_all",
            json.encodeToString(ListSerializer(BondedCompanion.serializer()), companions),
        )
    }

    override suspend fun savePendingBondedCompanion(value: BondedCompanion?) {
        write("bonded_pending", value?.let { json.encodeToString(BondedCompanion.serializer(), it) })
    }

    override suspend fun oauthTokens(): OAuthTokens? =
        read("oauth")?.let { json.decodeFromString(OAuthTokens.serializer(), it) }

    override suspend fun saveOauthTokens(value: OAuthTokens?) {
        write("oauth", value?.let { json.encodeToString(OAuthTokens.serializer(), it) })
    }

    override suspend fun bondedCompanions(): List<BondedCompanion> {
        val stored = read("bonded_all")?.let {
            json.decodeFromString(ListSerializer(BondedCompanion.serializer()), it)
        }
        return (stored ?: listOfNotNull(bondedCompanion()))
            .distinctBy { it.controllerIdB64 }
            .takeLast(MAX_MOBILE_RELAY_DEVICES)
    }

    override suspend fun mobileRelaySettings(): MobileRelaySettings? =
        read("mobile_relay")?.let {
            json.decodeFromString(MobileRelaySettings.serializer(), it)
        }

    override suspend fun saveMobileRelaySettings(value: MobileRelaySettings?) {
        write(
            "mobile_relay",
            value?.let { json.encodeToString(MobileRelaySettings.serializer(), it) },
        )
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

    companion object {
        private const val KEY_ALIAS = "kitsu.mobile.credentials.v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val IV_BYTES = 12
    }
}
