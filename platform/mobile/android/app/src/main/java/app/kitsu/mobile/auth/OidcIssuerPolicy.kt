package app.kitsu.mobile.auth

import java.net.URI

/** Pure-JVM validation shared by runtime code and exact contract tests. */
internal object OidcIssuerPolicy {
    const val OFFICIAL_ISSUER = "https://auth.k32.run/realms/kitsu"

    fun validationError(value: String): String? {
        val uri = runCatching { URI(value) }.getOrNull() ?: return "invalid_oidc_issuer"
        if (uri.scheme != "https") return "oidc_issuer_requires_https"
        if (uri.host.isNullOrBlank() || uri.rawUserInfo != null || uri.rawQuery != null || uri.rawFragment != null) {
            return "invalid_oidc_issuer"
        }
        if (uri.path.endsWith('/') && uri.path != "/") return "oidc_issuer_must_be_canonical"
        return null
    }

    fun discoveryUrl(issuer: String): String {
        check(validationError(issuer) == null) { "invalid_oidc_issuer" }
        return issuer.trimEnd('/') + "/.well-known/openid-configuration"
    }

    /** Tokens are never sent to a revocation endpoint on another origin. */
    fun isTrustedRevocationEndpoint(issuer: String, endpoint: String): Boolean {
        val issuerUri = runCatching { URI(issuer) }.getOrNull() ?: return false
        val endpointUri = runCatching { URI(endpoint) }.getOrNull() ?: return false
        return endpointUri.scheme == "https" && endpointUri.host == issuerUri.host &&
            effectivePort(endpointUri) == effectivePort(issuerUri) &&
            endpointUri.rawUserInfo == null && endpointUri.rawQuery == null &&
            endpointUri.rawFragment == null && endpointUri.path.isNotBlank()
    }

    private fun effectivePort(uri: URI): Int = if (uri.port == -1) 443 else uri.port
}
