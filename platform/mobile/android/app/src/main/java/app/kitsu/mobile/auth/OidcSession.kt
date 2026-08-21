package app.kitsu.mobile.auth

import android.content.Context
import android.content.Intent
import android.net.Uri
import app.kitsu.mobile.security.CredentialStore
import app.kitsu.mobile.security.OAuthTokens
import app.kitsu.mobile.security.SafeLog
import app.kitsu.mobile.transport.AccessTokenProvider
import app.kitsu.mobile.transport.TransportException
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import net.openid.appauth.AuthorizationException
import net.openid.appauth.AuthorizationRequest
import net.openid.appauth.AuthorizationResponse
import net.openid.appauth.AuthorizationService
import net.openid.appauth.AuthorizationServiceConfiguration
import net.openid.appauth.CodeVerifierUtil
import net.openid.appauth.NoClientAuthentication
import net.openid.appauth.ResponseTypeValues
import net.openid.appauth.TokenRequest
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import okhttp3.FormBody
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.OkHttpClient
import okhttp3.Request

data class OidcConfiguration(
    val issuer: String,
    val clientId: String,
    val redirectUri: String,
) {
    init {
        OidcIssuerPolicy.validationError(issuer)?.let { throw IllegalArgumentException(it) }
        require(clientId.isNotBlank()) { "oidc_client_id_required" }
        require(Uri.parse(redirectUri).scheme?.isNotBlank() == true) { "oidc_redirect_required" }
    }
}

@Serializable
private data class OidcDiscoveryMetadata(
    @SerialName("revocation_endpoint") val revocationEndpoint: String? = null,
)

class OidcSession(
    context: Context,
    private val configuration: OidcConfiguration,
    private val credentials: CredentialStore,
) : AccessTokenProvider {
    private val service = AuthorizationService(context.applicationContext)
    private val json = Json { ignoreUnknownKeys = true }
    private val http = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(5, TimeUnit.SECONDS)
        .callTimeout(10, TimeUnit.SECONDS)
        .build()

    suspend fun authorizationIntent(): Intent {
        val discovered = discover()
        val verifier = CodeVerifierUtil.generateRandomCodeVerifier()
        val challenge = CodeVerifierUtil.deriveCodeVerifierChallenge(verifier)
        val request = AuthorizationRequest.Builder(
            discovered,
            configuration.clientId,
            ResponseTypeValues.CODE,
            Uri.parse(configuration.redirectUri),
        )
            .setScope("openid profile offline_access kitsu.owner")
            .setCodeVerifier(verifier, challenge, CodeVerifierUtil.getCodeVerifierChallengeMethod())
            .build()
        return service.getAuthorizationRequestIntent(request)
    }

    suspend fun completeAuthorization(result: Intent): Boolean {
        val exception = AuthorizationException.fromIntent(result)
        val response = AuthorizationResponse.fromIntent(result)
        if (exception != null || response == null) {
            SafeLog.warn("oidc_authorization", "authorization_failed", exception)
            return false
        }
        val tokenResponse = exchange(response.createTokenExchangeRequest()) ?: return false
        val access = tokenResponse.accessToken ?: return false
        credentials.saveOauthTokens(
            OAuthTokens(
                accessToken = access,
                refreshToken = tokenResponse.refreshToken,
                accessTokenExpiresAtEpochSeconds =
                    (tokenResponse.accessTokenExpirationTime ?: (System.currentTimeMillis() + 300_000L)) / 1000L,
                idToken = tokenResponse.idToken,
            ),
        )
        return true
    }

    /** Reports only whether this installation currently holds an owner
     * session. It never returns or logs any token material. */
    suspend fun hasStoredSession(): Boolean = credentials.oauthTokens() != null

    override suspend fun accessToken(): String? {
        val saved = credentials.oauthTokens() ?: return null
        val now = System.currentTimeMillis() / 1000L
        if (saved.accessTokenExpiresAtEpochSeconds > now + 60L) return saved.accessToken
        val refreshToken = saved.refreshToken
        if (refreshToken == null) {
            credentials.saveOauthTokens(null)
            return null
        }
        val refreshed = refresh(refreshToken, saved)
        if (refreshed == null) credentials.saveOauthTokens(null)
        return refreshed
    }

    /**
     * Best-effort RFC 7009 revocation followed by mandatory local deletion.
     * Returns true only when every available token was acknowledged by the
     * issuer. Local sign-out succeeds even while the issuer is unavailable.
     */
    suspend fun signOut(): Boolean {
        val saved = credentials.oauthTokens()
        // Local authorization is removed before any fallible network work.
        credentials.saveOauthTokens(null)
        if (saved == null) return true
        try {
            return revoke(saved)
        } catch (failure: Throwable) {
            SafeLog.warn("oidc_revocation", "revocation_unavailable", failure)
            return false
        }
    }

    private suspend fun refresh(refreshToken: String, previous: OAuthTokens): String? {
        val discovered = discover()
        val request = TokenRequest.Builder(discovered, configuration.clientId)
            .setGrantType("refresh_token")
            .setRefreshToken(refreshToken)
            .build()
        val response = exchange(request) ?: return null
        val access = response.accessToken ?: return null
        credentials.saveOauthTokens(
            OAuthTokens(
                accessToken = access,
                refreshToken = response.refreshToken ?: previous.refreshToken,
                accessTokenExpiresAtEpochSeconds =
                    (response.accessTokenExpirationTime ?: (System.currentTimeMillis() + 300_000L)) / 1000L,
                idToken = response.idToken ?: previous.idToken,
            ),
        )
        return access
    }

    private suspend fun discover(): AuthorizationServiceConfiguration =
        suspendCancellableCoroutine { continuation ->
            AuthorizationServiceConfiguration.fetchFromIssuer(Uri.parse(configuration.issuer)) { config, error ->
                if (config != null) continuation.resume(config)
                else continuation.resumeWith(Result.failure(TransportException("oidc_discovery_failed", error)))
            }
        }

    private suspend fun revoke(tokens: OAuthTokens): Boolean = withContext(Dispatchers.IO) {
        val discoveryRequest = Request.Builder()
            .url(OidcIssuerPolicy.discoveryUrl(configuration.issuer).toHttpUrl())
            .get()
            .build()
        val metadata = http.newCall(discoveryRequest).execute().use { response ->
            if (!response.isSuccessful) throw IOException("oidc_discovery_http_${response.code}")
            val body = response.body ?: throw IOException("oidc_discovery_empty")
            if (body.contentLength() > MAX_DISCOVERY_BYTES) throw IOException("oidc_discovery_too_large")
            val source = body.source()
            source.request(MAX_DISCOVERY_BYTES + 1L)
            if (source.buffer.size > MAX_DISCOVERY_BYTES) throw IOException("oidc_discovery_too_large")
            json.decodeFromString<OidcDiscoveryMetadata>(source.readUtf8())
        }
        val endpoint = metadata.revocationEndpoint
            ?.takeIf { OidcIssuerPolicy.isTrustedRevocationEndpoint(configuration.issuer, it) }
            ?: return@withContext false
        val candidates = buildList {
            tokens.refreshToken?.let { add(it to "refresh_token") }
            add(tokens.accessToken to "access_token")
        }
        var allRevoked = true
        candidates.forEach { (token, hint) ->
            allRevoked = revokeToken(endpoint, token, hint) && allRevoked
        }
        allRevoked
    }

    private fun revokeToken(endpoint: String, token: String, hint: String): Boolean {
        val form = FormBody.Builder()
            .add("token", token)
            .add("token_type_hint", hint)
            .add("client_id", configuration.clientId)
            .build()
        val request = Request.Builder().url(endpoint).post(form).build()
        return http.newCall(request).execute().use { it.isSuccessful }
    }

    private suspend fun exchange(request: TokenRequest) = withContext(Dispatchers.Main) {
        suspendCancellableCoroutine { continuation ->
            service.performTokenRequest(request, NoClientAuthentication.INSTANCE) { response, error ->
                if (response != null) continuation.resume(response)
                else {
                    SafeLog.warn("oidc_token", "token_exchange_failed", error)
                    continuation.resume(null)
                }
            }
        }
    }

    private companion object {
        const val MAX_DISCOVERY_BYTES = 128L * 1024L
    }
}
